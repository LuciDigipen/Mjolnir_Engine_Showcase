/**
 * @file TerrainMeshResidency.cpp
 * @author Jackey Cen
 * @brief Keeps the GPU meshes of wanted terrain draw units within a GPU budget, built on workers, uploaded a frame at a
 * time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainMeshResidency.h"

#include <terrain/RunGuarded.h>
#include <terrain/TerrainLod.h>
#include <terrain/TerrainOverlayMesh.h>
#include <terrain/TerrainRegion.h>
#include <terrain/TerrainRenderMesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace mir
{
    namespace
    {
        using SteadyClock = std::chrono::steady_clock;

        double Seconds(SteadyClock::duration duration)
        {
            return std::chrono::duration<double>(duration).count();
        }

        float Rank(float rank)
        {
            return std::isnan(rank) ? std::numeric_limits<float>::infinity() : rank;
        }

        // Sooner first: higher demand, then lower rank, then older.
        struct Order
        {
            TerrainDemand demand = TerrainDemand::Prefetch;
            float rank = 0.0f;
            uint64_t ticket = 0;

            bool operator<(const Order &other) const
            {
                if (demand != other.demand)
                    return demand > other.demand;
                if (rank != other.rank)
                    return rank < other.rank;
                return ticket < other.ticket;
            }
        };

        struct BuildJob
        {
            uint64_t ticket = 0;
            TerrainPayloadKey geometry;
            std::shared_ptr<const TerrainReleaseHandle> release;
            std::shared_ptr<const TerrainLodSurface> lod;
            std::shared_ptr<const TerrainLodNormals> normals;
            std::shared_ptr<const TerrainRegionMesh> region;
            std::shared_ptr<const TerrainOverlay> overlay;
            std::vector<std::shared_ptr<const TileHeights>> sources;
        };

        // A mesh is one geometry payload of one release, or a snapshot's rebuild of a tile of it.
        struct MeshKey
        {
            TerrainPayloadKey geometry;
            std::shared_ptr<const TerrainOverlay> overlay;

            bool operator==(const MeshKey &) const = default;
        };

        struct MeshKeyHash
        {
            size_t operator()(const MeshKey &key) const noexcept
            {
                size_t h = std::hash<TerrainPayloadKey>{}(key.geometry);
                const size_t overlay = std::hash<const TerrainOverlay *>{}(key.overlay.get());
                return h ^ (overlay + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
            }
        };

        bool HoldsOverlay(const std::shared_ptr<const TerrainOverlay> &overlay, TileCoord tile)
        {
            if (!overlay)
                return false;
            const std::shared_ptr<const TileHeights> *held = overlay->Find(tile);
            return held && *held;
        }

        struct Finished
        {
            uint64_t ticket = 0;
            std::optional<TerrainMeshData> mesh;
            std::string error;
            double seconds = 0.0;
        };

        // Worker side: only the job's own payloads and the immutable index.
        Finished Build(const BuildJob &job)
        {
            const auto start = SteadyClock::now();
            Finished out;
            out.ticket = job.ticket;
            TerrainMeshData data;
            data.geometry = job.geometry;
            data.overlay = job.overlay;
            if (job.region)
            {
                const TerrainRegionMesh &region = *job.region;
                data.origin = region.origin;
                data.vertices.reserve(region.positions.size());
                for (size_t i = 0; i < region.positions.size(); ++i)
                    data.vertices.push_back({region.positions[i], region.normals[i]});
                data.indices = region.indices;
                out.mesh = std::move(data);
            }
            else
            {
                TerrainResult<TerrainTileMesh> built =
                    job.overlay ? BuildTerrainOverlayTileMesh(job.release->Index(), job.release->Info(),
                                                              job.geometry.coord, *job.lod, *job.overlay, job.sources)
                                : MakeTerrainRenderMesh(job.release->Index(), *job.lod, *job.normals);
                if (built)
                {
                    data.origin = built.value->origin;
                    data.vertices = std::move(built.value->vertices);
                    data.indices = std::move(built.value->indices);
                    out.mesh = std::move(data);
                }
                else
                    out.error = std::move(built.error.message);
            }
            out.seconds = Seconds(SteadyClock::now() - start);
            return out;
        }

        // Shared with every posted job, so a job that runs after the residency is gone finds a live queue.
        class BuildQueue
        {
          public:
            void Push(const Order &order, BuildJob job)
            {
                std::lock_guard lock(mMutex);
                if (mClosed)
                    return;
                mOrders[order.ticket] = order;
                mPending.emplace(order, std::move(job));
            }

            // False once a worker has taken it.
            bool Remove(uint64_t ticket)
            {
                std::lock_guard lock(mMutex);
                const auto it = mOrders.find(ticket);
                if (it == mOrders.end())
                    return false;
                mPending.erase(it->second);
                mOrders.erase(it);
                return true;
            }

            // A posted job builds whatever is soonest when it runs.
            void RunOne()
            {
                BuildJob job;
                {
                    std::lock_guard lock(mMutex);
                    if (mPending.empty())
                        return;
                    const auto first = mPending.begin();
                    mOrders.erase(first->first.ticket);
                    job = std::move(first->second);
                    mPending.erase(first);
                }
                Finished finished = RunGuarded([&] { return Build(job); }, [&](std::string_view what)
                    {
                        Finished failed;
                        failed.ticket = job.ticket;
                        failed.error = std::format("the mesh build threw: {}", what);
                        return failed;
                    });
                std::lock_guard lock(mMutex);
                if (!mClosed)
                    mFinished.push_back(std::move(finished));
            }

            std::deque<Finished> Take()
            {
                std::lock_guard lock(mMutex);
                return std::exchange(mFinished, {});
            }

            // Not started and not installed work is dropped.
            void Clear()
            {
                std::lock_guard lock(mMutex);
                mPending.clear();
                mOrders.clear();
                mFinished.clear();
            }

            void Close()
            {
                std::lock_guard lock(mMutex);
                mClosed = true;
                mPending.clear();
                mOrders.clear();
                mFinished.clear();
            }

          private:
            std::mutex mMutex;
            std::map<Order, BuildJob> mPending;
            std::unordered_map<uint64_t, Order> mOrders;
            std::deque<Finished> mFinished;
            bool mClosed = false;
        };

        bool SameRelease(const TerrainPayloadKey &key, const TerrainReleaseInfo &release)
        {
            return key.asset == release.asset && key.release == release.release;
        }
    } // namespace

    struct TerrainMeshResidency::Impl
    {
        struct Entry
        {
            MeshKey key;
            TerrainMeshState state = TerrainMeshState::Building;
            // Reserved from the build on.
            uint64_t bytes = 0;
            uint64_t ticket = 0;
            std::optional<TerrainMeshData> built;
            TerrainResidentMesh mesh;
            double lastWanted = -std::numeric_limits<double>::infinity();
            double lastUsed = -std::numeric_limits<double>::infinity();
            // This Request's want, if any.
            std::optional<TerrainDemand> demand;
            float rank = 0.0f;
            std::string error;
        };

        // Every mesh made for one geometry payload: the release's, and each snapshot's rebuild.
        struct Slot
        {
            std::vector<MeshKey> keys;
            // What the last Request asks for; null means the release's own mesh.
            std::shared_ptr<const TerrainOverlay> snapshot;
            bool tracked = false;
        };

        struct Want
        {
            TerrainUnitKind kind = TerrainUnitKind::Tile;
            TerrainPayloadKey normals;
            TerrainDemand demand = TerrainDemand::Prefetch;
            float rank = 0.0f;
        };

        TerrainStreamingSettings::Gpu settings;
        std::shared_ptr<TerrainExecutor> executor;
        TerrainMeshUploader &uploader;
        std::shared_ptr<BuildQueue> queue = std::make_shared<BuildQueue>();
        std::unordered_map<MeshKey, Entry, MeshKeyHash> entries;
        std::unordered_map<TerrainPayloadKey, Slot> slots;
        std::unordered_map<uint64_t, MeshKey> tickets;
        std::vector<std::pair<TerrainPayloadKey, std::string>> failures;
        uint64_t held = 0;
        uint64_t nextTicket = 0;
        TerrainMeshStats stats;

        Impl(const TerrainStreamingSettings::Gpu &s, std::shared_ptr<TerrainExecutor> e, TerrainMeshUploader &u)
            : settings(s), executor(std::move(e)), uploader(u)
        {
        }

        ~Impl()
        {
            queue->Close();
            executor.reset();
        }

        double LastTouched(const Entry &entry) const
        {
            return std::max(entry.lastWanted, entry.lastUsed);
        }

        void Attach(const MeshKey &key)
        {
            Slot &slot = slots[key.geometry];
            if (std::find(slot.keys.begin(), slot.keys.end(), key) == slot.keys.end())
                slot.keys.push_back(key);
        }

        void Detach(const MeshKey &key)
        {
            const auto slot = slots.find(key.geometry);
            if (slot == slots.end())
                return;
            std::erase(slot->second.keys, key);
            if (slot->second.keys.empty() && !slot->second.tracked)
                slots.erase(slot);
        }

        // The entry the last Request asked for, else the newest one that was wanted.
        const Entry *Preferred(const Slot &slot) const
        {
            const Entry *newest = nullptr;
            for (const MeshKey &key : slot.keys)
            {
                const auto found = entries.find(key);
                if (found == entries.end())
                    continue;
                const Entry &entry = found->second;
                if (entry.key.overlay == slot.snapshot)
                    return &entry;
                if (!newest || entry.lastWanted > newest->lastWanted)
                    newest = &entry;
            }
            return newest;
        }

        const Entry *FindDrawable(const Slot &slot) const
        {
            const Entry *newest = nullptr;
            for (const MeshKey &key : slot.keys)
            {
                const auto found = entries.find(key);
                if (found == entries.end() || found->second.state != TerrainMeshState::Ready)
                    continue;
                const Entry &entry = found->second;
                if (entry.key.overlay == slot.snapshot)
                    return &entry;
                if (!newest || entry.lastWanted > newest->lastWanted)
                    newest = &entry;
            }
            return newest;
        }

        Entry *FindDrawable(Slot &slot)
        {
            return const_cast<Entry *>(std::as_const(*this).FindDrawable(std::as_const(slot)));
        }

        // The entry goes; its GPU mesh, if any, is retired.
        void Drop(std::unordered_map<MeshKey, Entry, MeshKeyHash>::iterator it, bool retire)
        {
            Entry &entry = it->second;
            switch (entry.state)
            {
            case TerrainMeshState::Building:
                queue->Remove(entry.ticket);
                tickets.erase(entry.ticket);
                ++stats.buildsCanceled;
                break;
            case TerrainMeshState::Built:
                ++stats.buildsCanceled;
                break;
            case TerrainMeshState::Uploading:
            case TerrainMeshState::Ready:
                if (retire)
                {
                    uploader.Retire(entry.mesh.id);
                    ++stats.retired;
                }
                break;
            case TerrainMeshState::Absent:
            case TerrainMeshState::Failed:
                break;
            }
            held -= entry.bytes;
            Detach(entry.key);
            entries.erase(it);
        }

        void Fail(Entry &entry, const TerrainPayloadKey &key, std::string error)
        {
            held -= entry.bytes;
            entry.bytes = 0;
            entry.state = TerrainMeshState::Failed;
            entry.built.reset();
            entry.error = error;
            failures.emplace_back(key, std::move(error));
        }

        const TerrainTileRecord *GeometryRecord(const TerrainIndex &index, const TerrainPayloadKey &key) const
        {
            if (key.kind == TerrainPayloadKind::Region)
                return index.FindRegion({key.level, key.coord.x, key.coord.z});
            return index.Find(key.coord, key.layer, TerrainPayloadKind::LodHeight, key.level);
        }

        // Retires unwanted, then Prefetch meshes, farthest first, until bytes more fit; never a Visible one.
        void MakeRoom(TerrainDemand demand, uint64_t bytes)
        {
            std::vector<std::unordered_map<MeshKey, Entry, MeshKeyHash>::iterator> victims;
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                const Entry &entry = it->second;
                const bool resident =
                    entry.state == TerrainMeshState::Ready || entry.state == TerrainMeshState::Uploading;
                if (resident && (!entry.demand || *entry.demand < demand))
                    victims.push_back(it);
            }
            std::sort(victims.begin(), victims.end(), [this](const auto &a, const auto &b) {
                const Entry &x = a->second, &y = b->second;
                if (x.demand.has_value() != y.demand.has_value())
                    return !x.demand.has_value();
                if (!x.demand)
                    return LastTouched(x) < LastTouched(y);
                return x.rank > y.rank;
            });
            for (const auto &victim : victims)
            {
                if (held + bytes <= settings.budgetBytes)
                    return;
                ++stats.evicted;
                Drop(victim, true);
            }
        }
    };

    TerrainMeshResidency::TerrainMeshResidency(const TerrainStreamingSettings::Gpu &settings,
                                               std::shared_ptr<TerrainExecutor> executor, TerrainMeshUploader &uploader)
        : mImpl(std::make_unique<Impl>(settings, std::move(executor), uploader))
    {
        mImpl->stats.budgetBytes = settings.budgetBytes;
    }

    TerrainMeshResidency::~TerrainMeshResidency() = default;

    void TerrainMeshResidency::ApplySettings(const TerrainStreamingSettings::Gpu &gpu)
    {
        mImpl->settings.budgetBytes = gpu.budgetBytes;
        mImpl->settings.meshRetainSeconds = gpu.meshRetainSeconds;
        mImpl->stats.budgetBytes = gpu.budgetBytes;
    }

    void TerrainMeshResidency::Install()
    {
        Impl &impl = *mImpl;
        for (Finished &finished : impl.queue->Take())
        {
            const auto ticket = impl.tickets.find(finished.ticket);
            if (ticket == impl.tickets.end())
            {
                ++impl.stats.discarded;
                continue;
            }
            const MeshKey key = ticket->second;
            impl.tickets.erase(ticket);
            Impl::Entry &entry = impl.entries.at(key);
            impl.stats.buildSeconds += finished.seconds;
            impl.stats.maxBuildSeconds = std::max(impl.stats.maxBuildSeconds, finished.seconds);
            if (!finished.mesh)
            {
                impl.Fail(entry, key.geometry, std::move(finished.error));
                continue;
            }
            entry.built = std::move(finished.mesh);
            entry.state = TerrainMeshState::Built;
        }

        for (auto &[key, entry] : impl.entries)
        {
            if (entry.state != TerrainMeshState::Uploading)
                continue;
            switch (impl.uploader.State(entry.mesh.id))
            {
            case TerrainGpuMeshState::Uploading:
                break;
            case TerrainGpuMeshState::Ready:
                entry.state = TerrainMeshState::Ready;
                ++impl.stats.uploadsReady;
                break;
            case TerrainGpuMeshState::Failed:
                impl.uploader.Retire(entry.mesh.id);
                impl.Fail(entry, key.geometry, "the GPU upload failed");
                break;
            }
        }
    }

    void TerrainMeshResidency::Request(std::span<const TerrainSelectionInstance> instances,
                                       std::span<const TerrainPayloadWant> wants, const TerrainStreamer &streamer,
                                       double now)
    {
        Impl &impl = *mImpl;
        const auto start = SteadyClock::now();

        std::unordered_map<TerrainPayloadKey, Impl::Want> merged;
        for (const TerrainPayloadWant &want : wants)
        {
            if (want.key.kind != TerrainPayloadKind::LodHeight && want.key.kind != TerrainPayloadKind::Region)
                continue;
            Impl::Want next;
            next.kind = want.key.kind == TerrainPayloadKind::Region ? TerrainUnitKind::Region : TerrainUnitKind::Tile;
            next.normals = want.key;
            if (next.kind == TerrainUnitKind::Tile)
                next.normals.kind = TerrainPayloadKind::Normal;
            next.demand = want.demand;
            next.rank = Rank(want.rank);
            const auto [at, inserted] = merged.try_emplace(want.key, next);
            if (inserted)
                continue;
            if (next.demand > at->second.demand)
                at->second = next;
            else if (next.demand == at->second.demand)
                at->second.rank = std::min(at->second.rank, next.rank);
        }

        // The snapshot a geometry comes from: the tile's, when an instance's overlay holds it.
        std::unordered_map<TerrainPayloadKey, std::shared_ptr<const TerrainOverlay>> snapshots;
        for (const auto &[key, want] : merged)
        {
            std::shared_ptr<const TerrainOverlay> overlay;
            if (key.kind == TerrainPayloadKind::LodHeight && key.level == 0)
            {
                const auto instance =
                    std::find_if(instances.begin(), instances.end(), [&](const TerrainSelectionInstance &i) {
                        return i.release && SameRelease(key, i.release->Info());
                    });
                if (instance != instances.end() && HoldsOverlay(instance->overlay, key.coord))
                    overlay = instance->overlay;
            }
            snapshots.emplace(key, std::move(overlay));
        }

        for (auto &[geometry, slot] : impl.slots)
            slot.tracked = false;
        for (const auto &[key, snapshot] : snapshots)
        {
            Impl::Slot &slot = impl.slots[key];
            slot.tracked = true;
            slot.snapshot = snapshot;
        }

        for (auto &[key, entry] : impl.entries)
            entry.demand.reset();
        for (const auto &[key, want] : merged)
        {
            const auto snapshot = snapshots.find(key);
            if (snapshot == snapshots.end())
                continue;
            const auto found = impl.entries.find(MeshKey{key, snapshot->second});
            if (found == impl.entries.end())
                continue;
            found->second.demand = want.demand;
            found->second.rank = want.rank;
            found->second.lastWanted = now;
        }

        // An older snapshot's mesh retires once the wanted snapshot's mesh can be drawn.
        const auto superseded = [&impl](const MeshKey &key) {
            const auto slot = impl.slots.find(key.geometry);
            if (slot == impl.slots.end() || !slot->second.tracked)
                return false;
            const Impl::Entry *wanted = impl.Preferred(slot->second);
            if (!wanted || wanted->state != TerrainMeshState::Ready)
                return false;
            return !(wanted->key == key);
        };

        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            const Impl::Entry &entry = it->second;
            const bool building = entry.state == TerrainMeshState::Building || entry.state == TerrainMeshState::Built;
            const bool resident = entry.state == TerrainMeshState::Uploading || entry.state == TerrainMeshState::Ready;
            const bool stale = !entry.demand && now - impl.LastTouched(entry) >= impl.settings.meshRetainSeconds;
            if (superseded(entry.key) || (building && !entry.demand) || (resident && stale))
                impl.Drop(it++, true);
            else
                ++it;
        }

        struct Candidate
        {
            TerrainPayloadKey geometry;
            std::shared_ptr<const TerrainOverlay> overlay;
            const Impl::Want *want = nullptr;
            const TerrainSelectionInstance *instance = nullptr;
        };
        std::vector<Candidate> candidates;
        for (const auto &[key, want] : merged)
        {
            const auto snapshot = snapshots.find(key);
            const std::shared_ptr<const TerrainOverlay> overlay =
                snapshot == snapshots.end() ? nullptr : snapshot->second;
            if (impl.entries.contains(MeshKey{key, overlay}))
                continue;
            const auto instance =
                std::find_if(instances.begin(), instances.end(), [&](const TerrainSelectionInstance &i) {
                    return i.release && SameRelease(key, i.release->Info());
                });
            if (instance == instances.end())
                continue;
            bool resident = streamer.Status(key).state == TerrainPayloadState::Ready;
            if (want.kind != TerrainUnitKind::Region && !overlay)
                resident = resident && streamer.Status(want.normals).state == TerrainPayloadState::Ready;
            if (want.kind != TerrainUnitKind::Region && overlay)
                for (const TileCoord source : TerrainOverlaySourceTiles(instance->release->Index(), key.coord))
                    resident = resident &&
                               streamer.Status(TerrainPayloadKeyOf(instance->release->Info(),
                                                                   *instance->release->Index().Find(source)))
                                       .state == TerrainPayloadState::Ready;
            if (resident)
                candidates.push_back({key, overlay, &want, &*instance});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return std::tie(b.want->demand, a.want->rank) < std::tie(a.want->demand, b.want->rank);
        });

        const uint64_t frameBudget = impl.uploader.FrameBudgetBytes();
        uint64_t building = 0;
        for (const auto &[key, entry] : impl.entries)
            if (entry.state == TerrainMeshState::Building || entry.state == TerrainMeshState::Built)
                building += entry.bytes;

        for (const Candidate &candidate : candidates)
        {
            const TerrainReleaseHandle &release = *candidate.instance->release;
            const TerrainTileRecord *record = impl.GeometryRecord(release.Index(), candidate.geometry);
            if (!record)
                continue;
            const uint64_t bytes = impl.uploader.Footprint(record->vertexCount, record->indexCount);
            if (building > 0 && building + bytes > frameBudget)
                break;
            if (impl.held + bytes > impl.settings.budgetBytes)
                impl.MakeRoom(candidate.want->demand, bytes);
            if (impl.held + impl.uploader.RetiringBytes() + bytes > impl.settings.budgetBytes)
            {
                ++impl.stats.budgetWaits;
                break;
            }

            BuildJob job;
            job.geometry = candidate.geometry;
            job.release = candidate.instance->release;
            job.overlay = candidate.overlay;
            if (candidate.want->kind == TerrainUnitKind::Region)
                job.region = streamer.Region(candidate.geometry);
            else
            {
                job.lod = streamer.Lod(candidate.geometry);
                if (candidate.overlay)
                {
                    bool complete = true;
                    const TerrainIndex &index = job.release->Index();
                    for (const TileCoord source : TerrainOverlaySourceTiles(index, candidate.geometry.coord))
                    {
                        std::shared_ptr<const TileHeights> heights =
                            streamer.Heights(TerrainPayloadKeyOf(job.release->Info(), *index.Find(source)));
                        if (!heights)
                        {
                            complete = false;
                            break;
                        }
                        job.sources.push_back(std::move(heights));
                    }
                    if (!complete)
                        continue;
                }
                else
                    job.normals = streamer.Normals(candidate.want->normals);
            }
            if (!job.region && (!job.lod || (!job.overlay && !job.normals)))
                continue;
            job.ticket = impl.nextTicket++;

            const MeshKey meshKey{candidate.geometry, candidate.overlay};
            Impl::Entry &entry = impl.entries[meshKey];
            entry.key = meshKey;
            entry.state = TerrainMeshState::Building;
            entry.bytes = bytes;
            entry.ticket = job.ticket;
            entry.demand = candidate.want->demand;
            entry.rank = candidate.want->rank;
            entry.lastWanted = now;
            impl.held += bytes;
            building += bytes;
            impl.Attach(meshKey);
            impl.tickets[job.ticket] = meshKey;
            impl.queue->Push({candidate.want->demand, candidate.want->rank, job.ticket}, std::move(job));
            impl.executor->Post([queue = impl.queue] { queue->RunOne(); });
            ++impl.stats.buildsStarted;
        }

        std::vector<std::pair<Order, MeshKey>> built;
        for (const auto &[key, entry] : impl.entries)
            if (entry.state == TerrainMeshState::Built)
                built.push_back({{entry.demand.value_or(TerrainDemand::Prefetch), entry.rank, entry.ticket}, key});
        std::sort(built.begin(), built.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[order, key] : built)
        {
            if (impl.uploader.QueuedBytes() >= frameBudget)
                break;
            Impl::Entry &entry = impl.entries.at(key);
            const TerrainLocalPosition origin = entry.built->origin;
            const TerrainGpuMeshId id = impl.uploader.Upload(std::move(*entry.built));
            entry.built.reset();
            if (id == 0)
            {
                impl.Fail(entry, key.geometry, "the GPU upload could not be queued");
                continue;
            }
            entry.mesh = {id, origin};
            entry.state = TerrainMeshState::Uploading;
            ++impl.stats.uploadsQueued;
        }

        impl.stats.peakBytes = std::max(impl.stats.peakBytes, impl.held + impl.uploader.RetiringBytes());
        impl.stats.lastRequestSeconds = Seconds(SteadyClock::now() - start);
        impl.stats.maxRequestSeconds = std::max(impl.stats.maxRequestSeconds, impl.stats.lastRequestSeconds);
    }

    TerrainMeshState TerrainMeshResidency::State(const TerrainPayloadKey &geometry) const
    {
        const auto slot = mImpl->slots.find(geometry);
        if (slot == mImpl->slots.end())
            return TerrainMeshState::Absent;
        const Impl::Entry *entry = mImpl->Preferred(slot->second);
        return entry == nullptr ? TerrainMeshState::Absent : entry->state;
    }

    bool TerrainMeshResidency::Ready(const TerrainPayloadKey &geometry) const
    {
        const auto slot = mImpl->slots.find(geometry);
        return slot != mImpl->slots.end() && mImpl->FindDrawable(slot->second) != nullptr;
    }

    const TerrainResidentMesh *TerrainMeshResidency::Acquire(const TerrainPayloadKey &geometry, double now)
    {
        const auto slot = mImpl->slots.find(geometry);
        if (slot == mImpl->slots.end())
            return nullptr;
        Impl::Entry *entry = mImpl->FindDrawable(slot->second);
        if (entry == nullptr)
            return nullptr;
        entry->lastUsed = now;
        return &entry->mesh;
    }

    const TerrainResidentMesh *TerrainMeshResidency::FindReady(const TerrainPayloadKey &geometry) const
    {
        const auto slot = mImpl->slots.find(geometry);
        if (slot == mImpl->slots.end())
            return nullptr;
        const Impl::Entry *entry = mImpl->FindDrawable(slot->second);
        return entry == nullptr ? nullptr : &entry->mesh;
    }

    void TerrainMeshResidency::RetireReleases(std::span<const TerrainReleaseInfo> keep)
    {
        Impl &impl = *mImpl;
        for (auto it = impl.entries.begin(); it != impl.entries.end();)
        {
            const bool kept = std::any_of(keep.begin(), keep.end(), [&](const TerrainReleaseInfo &release) {
                return SameRelease(it->first.geometry, release);
            });
            if (kept)
                ++it;
            else
                impl.Drop(it++, true);
        }
        std::erase_if(impl.slots, [&](const auto &slot) {
            return !std::any_of(keep.begin(), keep.end(), [&](const TerrainReleaseInfo &release) {
                return SameRelease(slot.first, release);
            });
        });
    }

    void TerrainMeshResidency::RetireAll()
    {
        RetireReleases({});
    }

    void TerrainMeshResidency::Forget()
    {
        Impl &impl = *mImpl;
        impl.queue->Clear();
        impl.tickets.clear();
        impl.entries.clear();
        impl.slots.clear();
        impl.held = 0;
    }

    std::vector<std::pair<TerrainPayloadKey, std::string>> TerrainMeshResidency::TakeFailures()
    {
        return std::exchange(mImpl->failures, {});
    }

    TerrainMeshStats TerrainMeshResidency::Stats() const
    {
        const Impl &impl = *mImpl;
        TerrainMeshStats stats = impl.stats;
        stats.heldBytes = impl.held;
        stats.retiringBytes = impl.uploader.RetiringBytes();
        for (const auto &[key, entry] : impl.entries)
            switch (entry.state)
            {
            case TerrainMeshState::Building:
                ++stats.building;
                break;
            case TerrainMeshState::Built:
                ++stats.built;
                break;
            case TerrainMeshState::Uploading:
                ++stats.uploading;
                break;
            case TerrainMeshState::Ready:
                ++stats.ready;
                break;
            case TerrainMeshState::Failed:
                ++stats.failed;
                break;
            case TerrainMeshState::Absent:
                break;
            }
        return stats;
    }

    TerrainReleaseUsage TerrainMeshResidency::Usage(const TerrainReleaseInfo &release) const
    {
        TerrainReleaseUsage usage;
        for (const auto &[key, entry] : mImpl->entries)
        {
            if (!SameRelease(key.geometry, release))
                continue;
            usage.bytes += entry.bytes;
            switch (entry.state)
            {
            case TerrainMeshState::Building:
            case TerrainMeshState::Built:
            case TerrainMeshState::Uploading:
                ++usage.inFlight;
                break;
            case TerrainMeshState::Ready:
                ++usage.ready;
                break;
            case TerrainMeshState::Failed:
                if (usage.failed++ == 0)
                    usage.failure = std::format("{}: {}", DescribeTerrainPayloadKey(key.geometry), entry.error);
                break;
            case TerrainMeshState::Absent:
                break;
            }
        }
        return usage;
    }
} // namespace mir
