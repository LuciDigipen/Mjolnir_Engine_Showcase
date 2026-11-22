/**
 * @file ImGuiUIBackend.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui_imgui/ImGuiUIBackend: cooked textures reaching ImGui's
 *        dynamic texture path, loading through AssetCache, teardown before and
 *        after the context, and EndFrame drawing a sprite as an image; fonts
 *        in ImGui's atlas, drawn and measured with their own glyphs. Runs a
 *        headless ImGui context with no renderer backend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_imgui/ImGuiUIBackend.h>

#include <gameui/UIContext.h>
#include <gameui/UIFonts.h>
#include <gameui/UITextures.h>
#include <gameui/UIWidgets.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <assets/ModelAsset.h>
#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    // a headless context; nothing uploads, so a texture stays in WantCreate
    class ImGuiFixture
    {
    public:
        ImGuiFixture()
        {
            mContext = ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }

        ~ImGuiFixture() { Destroy(); }

        ImGuiFixture(const ImGuiFixture &) = delete;
        ImGuiFixture &operator=(const ImGuiFixture &) = delete;

        void Destroy()
        {
            if (mContext != nullptr)
                ImGui::DestroyContext(mContext);
            mContext = nullptr;
        }

        // one ImGui frame through Render, which lists the textures for the backend
        void Render()
        {
            ImGui::NewFrame();
            ImGui::Render();
        }

    private:
        ImGuiContext *mContext = nullptr;
    };

    // a w x h texture whose texel n holds n in every channel
    TextureAsset Numbered(uint32_t width, uint32_t height)
    {
        TextureAsset texture{width, height, {}};
        for (uint32_t i = 0; i < width * height * 4; ++i)
            texture.rgba.push_back(static_cast<uint8_t>(i / 4));
        return texture;
    }

    // the listed texture of that size, which the backend would be handed
    ImTextureData *Listed(int width, int height)
    {
        for (ImTextureData *texture : ImGui::GetPlatformIO().Textures)
            if (texture->Width == width && texture->Height == height)
                return texture;
        return nullptr;
    }

    // catch_discover_tests runs each TEST_CASE in its own process, so the
    // directory needs a per-process token as well as a counter
    struct ScratchDir
    {
        std::filesystem::path path;

        ScratchDir()
        {
            static std::atomic<int> counter{0};
            std::random_device rd;
            std::ostringstream token;
            token << std::hex << ((static_cast<uint64_t>(rd()) << 32) ^ rd());
            path = std::filesystem::temp_directory_path() /
                   ("mjolnir_uitextures_test_" + token.str() + "_" + std::to_string(counter.fetch_add(1)));
            std::filesystem::create_directories(path);
        }

        ~ScratchDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        ScratchDir(const ScratchDir &) = delete;
        ScratchDir &operator=(const ScratchDir &) = delete;
    };

    void WriteFile(const std::filesystem::path &path, const std::vector<std::byte> &bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<uint8_t> ReadTestFont()
    {
        std::ifstream file(MIR_TEST_FONT, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
} // namespace

TEST_CASE("Add hands the pixels to ImGui as a user texture for the backend to create", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UITextures textures;
    textures.SetBackend(&backend);

    const UISprite sprite = textures.Add(Numbered(3, 2));
    REQUIRE(sprite.IsValid());
    CHECK(textures.Count() == 1);
    CHECK(sprite.mTextureWidth == 3.0f);
    CHECK(sprite.mTextureHeight == 2.0f);
    CHECK(sprite.mSource == UIRect{0, 0, 3, 2});
    CHECK(sprite.mSlice == UIEdges{});
    CHECK(textures.Find(sprite.mTexture) != nullptr);
    CHECK(textures.Find(UITexture{}) == nullptr);
    CHECK(textures.Find(UITexture{2}) == nullptr);

    imgui.Render();
    ImTextureData *listed = Listed(3, 2);
    REQUIRE(listed != nullptr);
    CHECK(listed->Status == ImTextureStatus_WantCreate);
    CHECK(listed->Format == ImTextureFormat_RGBA32);
    CHECK(listed->TexID == ImTextureID_Invalid);
    const auto *pixels = static_cast<const uint8_t *>(listed->GetPixels());
    CHECK(pixels[0] == 0);
    CHECK(pixels[4 * 5 + 2] == 5);
}

TEST_CASE("Without an ImGui context a texture has no ImGui copy, and a wrong pixel count no sprite", "[gameui]")
{
    ImGuiUIBackend backend;
    UITextures textures;
    textures.SetBackend(&backend);
    const UISprite kept = textures.Add(Numbered(2, 2));
    REQUIRE(kept.IsValid());
    CHECK(textures.Find(kept.mTexture) == nullptr);

    ImGuiFixture imgui;
    TextureAsset truncated = Numbered(2, 2);
    truncated.rgba.pop_back();
    CHECK_FALSE(textures.Add(truncated).IsValid());
    CHECK(textures.Count() == 1);
}

TEST_CASE("A texture added before the backend is made when the backend is installed", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UITextures textures;
    const UISprite sprite = textures.Add(Numbered(6, 6));
    CHECK(textures.Find(sprite.mTexture) == nullptr);

    textures.SetBackend(&backend);
    CHECK(textures.Find(sprite.mTexture) != nullptr);
    imgui.Render();
    CHECK(Listed(6, 6) != nullptr);

    // removing the backend leaves the copy to the textures
    textures.SetBackend(nullptr);
    CHECK(Listed(6, 6) != nullptr);
}

TEST_CASE("UITextures destroyed with its context alive unregisters what was never uploaded", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    {
        UITextures textures;
        textures.SetBackend(&backend);
        textures.Add(Numbered(5, 7));
        imgui.Render();
        REQUIRE(Listed(5, 7) != nullptr);
    }

    // gone from the list at once, not just at the next render
    CHECK(Listed(5, 7) == nullptr);
    imgui.Render();
    CHECK(Listed(5, 7) == nullptr);
}

TEST_CASE("UITextures outlives the context its textures were registered with", "[gameui]")
{
    // a backend still installed when ImGui went: nothing left to unregister
    ImGuiUIBackend backend;
    UITextures textures;
    textures.SetBackend(&backend);
    {
        ImGuiFixture imgui;
        textures.Add(Numbered(4, 4));
        imgui.Render();
    }
    CHECK(textures.Count() == 1);
}

TEST_CASE("Load reads a cooked texture through the asset cache, once per id", "[gameui]")
{
    ScratchDir dir;
    const AssetId texture = MakeAssetId("assets/ui/frame.png");
    const AssetId raw = MakeAssetId("assets/ui/notes.txt");
    const AssetId missing = MakeAssetId("assets/ui/missing.png");
    WriteFile(dir.path / "frame.blob", BuildTypedAsset(Numbered(6, 4)));
    const std::byte text[] = {std::byte{'h'}, std::byte{'i'}};
    WriteFile(dir.path / "notes.blob", BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, text));

    const AssetManifestSourceEntry entries[] = {
        {{texture, kTextureAssetTypeTag, kTextureAssetContentVersion}, "frame.blob"},
        {{raw, kRawAssetTypeTag, kRawAssetContentVersion}, "notes.blob"},
    };
    const std::optional<AssetManifest> manifest = AssetManifest::Parse(BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());
    AssetCache cache(&*manifest, dir.path);

    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UITextures textures;
    textures.SetBackend(&backend);
    const UISprite sprite = textures.Load(cache, texture);
    REQUIRE(sprite.IsValid());
    CHECK(sprite.mSource == UIRect{0, 0, 6, 4});
    CHECK(textures.Load(cache, texture) == sprite);
    CHECK(textures.Count() == 1);

    // not textures: each a null sprite, and nothing added
    CHECK_FALSE(textures.Load(cache, raw).IsValid());
    CHECK_FALSE(textures.Load(cache, missing).IsValid());
    CHECK_FALSE(textures.Load(cache, missing).IsValid());
    CHECK(textures.Count() == 1);
}

TEST_CASE("EndFrame draws a sprite as an image of its ImGui texture", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UIContext ui;
    ui.SetBackend(&backend);
    InputState input;
    const InputMap map = DefaultInputMap();

    UISprite sprite = ui.Textures().Add(Numbered(24, 24));
    sprite.mSlice = UIEdges{8, 8, 8, 8};
    ui.Style().mWidgetSprite = sprite;

    ImGui::NewFrame();
    ui.BeginFrame(UIRect{0, 0, 800, 600}, input, map, UIPointer{700, 550, false});
    Button(ui, UIId("button"), UIRect{100, 100, 200, 40}, "Play");
    ui.EndFrame();
    ImGui::Render();

    ImTextureData *listed = Listed(24, 24);
    REQUIRE(listed != nullptr);

    // the dynamic path: draw commands refer to the texture itself, not to an id
    // made for it by hand
    // by viewport: outside a frame there is no current window to take it from
    size_t images = 0;
    const ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    for (const ImDrawCmd &command : drawList->CmdBuffer)
        if (command.TexRef._TexData == listed)
            ++images;
    CHECK(images >= 1);
    CHECK(drawList->VtxBuffer.Size >= 9 * 4);
}

TEST_CASE("EndFrame draws into an area below the design size at the UI's scale, text included", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UIContext ui;
    ui.SetBackend(&backend);
    InputState input;
    const InputMap map = DefaultInputMap();

    ImGui::NewFrame();
    ui.BeginFrame(UIRect{0, 0, 320, 240}, input, map, UIPointer{});
    REQUIRE(ui.Scale() == 0.5f);
    Button(ui, UIId("button"), UIRect{100, 100, 200, 40}, "Play");
    ui.EndFrame();
    ImGui::Render();

    // at half size the face spans (50, 50) to (150, 70), and its text lies on it;
    // a pixel over for the anti-aliased edge
    const ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    REQUIRE(drawList->VtxBuffer.Size > 4);
    for (const ImDrawVert &vertex : drawList->VtxBuffer)
    {
        CHECK(vertex.pos.x >= 49.0f);
        CHECK(vertex.pos.y >= 49.0f);
        CHECK(vertex.pos.x <= 151.0f);
        CHECK(vertex.pos.y <= 71.0f);
    }
}

TEST_CASE("Text keeps to the box sized for it, whatever ImGui's DPI scale and at a scaled-up UI", "[gameui]")
{
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UIContext ui;
    ui.SetBackend(&backend);
    InputState input;
    const InputMap map = DefaultInputMap();

    // as the editor does on a 200% monitor; the game UI's text must not follow it
    ImGui::GetStyle().FontScaleDpi = 2.0f;

    ImGui::NewFrame();
    ui.BeginFrame(UIRect{0, 0, 1920, 1080}, input, map, UIPointer{});
    REQUIRE(ui.Scale() == Catch::Approx(1.5f));

    // measured with ImGui's own font at the size it is drawn
    const std::string_view text = "Escape";
    const UISize size = ui.MeasureText(text);
    const ImVec2 imguiSize = ImGui::GetFont()->CalcTextSizeA(16.0f * 1.5f, FLT_MAX, 0.0f, text.data(),
                                                             text.data() + text.size());
    CHECK(size.width * 1.5f == Catch::Approx(imguiSize.x));
    CHECK(size.height == Catch::Approx(16.0f).margin(1.0f));

    const float padding = ui.Style().mPadding;
    const UIRect rect{100, 100, size.width + 2.0f * padding, 32};
    Button(ui, UIId("button"), rect, text);
    ui.EndFrame();
    ImGui::Render();

    // the face, in pixels, holds every vertex: its own and its text's; a pixel over for the anti-aliased edge
    const ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    REQUIRE(drawList->VtxBuffer.Size > 4);
    for (const ImDrawVert &vertex : drawList->VtxBuffer)
    {
        CHECK(vertex.pos.x >= rect.x * 1.5f - 1.0f);
        CHECK(vertex.pos.y >= rect.y * 1.5f - 1.0f);
        CHECK(vertex.pos.x <= rect.Right() * 1.5f + 1.0f);
        CHECK(vertex.pos.y <= rect.Bottom() * 1.5f + 1.0f);
    }
}

TEST_CASE("A font goes into ImGui's atlas, draws and measures with its own glyphs, and leaves with UIFonts",
          "[gameui][fonts]")
{
    const std::vector<uint8_t> bytes = ReadTestFont();
    REQUIRE(bytes.size() > 1000);

    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    InputState input;
    const InputMap map = DefaultInputMap();

    // ImGui's own font first, as the editor loads Inter before the game UI runs
    ImGui::NewFrame();
    ImFontAtlas &atlas = *ImGui::GetIO().Fonts;
    const int before = atlas.Fonts.Size;
    REQUIRE(before >= 1);
    {
        UIContext ui;
        ui.SetBackend(&backend);
        const UIFont font = ui.Fonts().Add(bytes);
        REQUIRE(font.IsValid());
        REQUIRE(atlas.Fonts.Size == before + 1);
        ImFont *added = atlas.Fonts.back();

        ui.BeginFrame(UIRect{0, 0, 640, 480}, input, map, UIPointer{});
        const std::string_view text = "Altitude 14419";
        const UISize inFont = ui.MeasureText(text, font, 24.0f);
        const ImVec2 expected = added->CalcTextSizeA(24.0f, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
        CHECK(inFont.width == Catch::Approx(expected.x));
        CHECK(inFont.width != Catch::Approx(ui.MeasureText(text, UIFont{}, 24.0f).width));

        ui.Style().mFont = font;
        ui.Style().mFontSize = 24.0f;
        Label(ui, UIId("altitude"), UIRect{100, 100, 300, 40}, text);
        ui.EndFrame();
        ImGui::Render();

        const ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
        CHECK(drawList->VtxBuffer.Size >= 4 * 10);
    }
    CHECK(atlas.Fonts.Size == before);
}

TEST_CASE("UIFonts outlives the context its fonts were added to", "[gameui][fonts]")
{
    const std::vector<uint8_t> bytes = ReadTestFont();
    ImGuiFixture imgui;
    ImGuiUIBackend backend;
    UIFonts fonts;
    fonts.SetBackend(&backend);
    REQUIRE(fonts.Add(bytes).IsValid());

    // the atlas took the font with it; the release only drops what UIFonts kept
    imgui.Destroy();
    fonts.SetBackend(nullptr);
}

TEST_CASE("Load reads a font through the asset cache, once per id", "[gameui][fonts]")
{
    ScratchDir dir;
    const AssetId font = MakeAssetId("assets/fonts/Inter.ttf");
    const AssetId raw = MakeAssetId("assets/fonts/notes.txt");
    const AssetId missing = MakeAssetId("assets/fonts/missing.ttf");
    const std::vector<uint8_t> bytes = ReadTestFont();
    WriteFile(dir.path / "font.blob", BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion,
                                                     std::as_bytes(std::span(bytes))));
    const std::byte text[] = {std::byte{'h'}, std::byte{'i'}};
    WriteFile(dir.path / "notes.blob", BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, text));

    const AssetManifestSourceEntry entries[] = {
        {{font, kRawAssetTypeTag, kRawAssetContentVersion}, "font.blob"},
        {{raw, kRawAssetTypeTag, kRawAssetContentVersion}, "notes.blob"},
    };
    const std::optional<AssetManifest> manifest = AssetManifest::Parse(BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());
    AssetCache cache(&*manifest, dir.path);

    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    UIFonts fonts;
    fonts.SetBackend(&backend);
    const UIFont loaded = fonts.Load(cache, font);
    REQUIRE(loaded.IsValid());
    CHECK(fonts.Find(loaded) != nullptr);
    CHECK(fonts.Load(cache, font) == loaded);
    CHECK(fonts.Count() == 1);

    // not fonts: the default font each time, and nothing added
    CHECK_FALSE(fonts.Load(cache, raw).IsValid());
    CHECK_FALSE(fonts.Load(cache, missing).IsValid());
    CHECK_FALSE(fonts.Load(cache, missing).IsValid());
    CHECK(fonts.Count() == 1);
    ImGui::Render();
}

TEST_CASE("A damaged or cut-off font is refused before ImGui asserts on it", "[gameui][fonts]")
{
    const std::vector<uint8_t> bytes = ReadTestFont();
    REQUIRE(bytes.size() > 1000);

    // a whole directory whose tables ImGui cannot find, so stb_truetype refuses it
    std::vector<uint8_t> damaged = bytes;
    const size_t tables = (size_t{damaged[4]} << 8) | damaged[5];
    REQUIRE(12 + tables * 16 < damaged.size());
    for (size_t table = 0; table < tables; ++table)
        std::memcpy(&damaged[12 + table * 16], "zzzz", 4);

    // its last tables past the end
    const std::vector<uint8_t> cutOff(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));

    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    const int before = ImGui::GetIO().Fonts->Fonts.Size;
    UIFonts fonts;
    fonts.SetBackend(&backend);
    CHECK(backend.MakeFont(damaged).font == nullptr);
    CHECK_FALSE(fonts.Add(damaged).IsValid());
    CHECK_FALSE(fonts.Add(cutOff).IsValid());
    CHECK(fonts.Count() == 0);
    CHECK(ImGui::GetIO().Fonts->Fonts.Size == before);

    // and the font it was made from still loads
    CHECK(fonts.Add(bytes).IsValid());
    ImGui::Render();
}

TEST_CASE("Text under a pixel tall draws and measures at one pixel, as ImGui keeps it", "[gameui][fonts]")
{
    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    InputState input;
    const InputMap map = DefaultInputMap();

    UIContext ui;
    ui.SetBackend(&backend);
    const UIFont font = ui.Fonts().Add(ReadTestFont());
    REQUIRE(font.IsValid());

    const std::string_view text = "Speed";
    for (void *backendFont : {static_cast<void *>(nullptr), ui.Fonts().Find(font)})
    {
        const UISize tiny = backend.MeasureText(text, 0.25f, backendFont);
        const UISize onePixel = backend.MeasureText(text, 1.0f, backendFont);
        CHECK(tiny.width == Catch::Approx(onePixel.width));
        CHECK(tiny.height == Catch::Approx(onePixel.height));
        CHECK(tiny.height > 0.0f);
    }

    ui.BeginFrame(UIRect{0, 0, 1920, 1080}, input, map, UIPointer{});
    CHECK(ui.MeasureText(text, font, 0.25f).height > 0.0f);
    ui.Style().mFontSize = 0.25f;
    Label(ui, UIId("tiny"), UIRect{100, 100, 300, 40}, text);
    ui.Style().mFont = font;
    Label(ui, UIId("tiny_font"), UIRect{100, 200, 300, 40}, text);
    ui.EndFrame();
    ImGui::Render();
}

namespace
{
    constexpr UIRect kScreen{0, 0, 800, 600};

    struct VertexBounds
    {
        ImVec2 mMin{FLT_MAX, FLT_MAX};
        ImVec2 mMax{-FLT_MAX, -FLT_MAX};
        ImVec2 mUVMin{FLT_MAX, FLT_MAX};
        ImVec2 mUVMax{-FLT_MAX, -FLT_MAX};
    };

    VertexBounds BoundsOf(const ImDrawList &drawn, int first, int last)
    {
        VertexBounds bounds;
        for (int i = first; i < last; ++i)
        {
            const ImDrawVert &vertex = drawn.VtxBuffer[i];
            bounds.mMin = ImVec2(std::min(bounds.mMin.x, vertex.pos.x), std::min(bounds.mMin.y, vertex.pos.y));
            bounds.mMax = ImVec2(std::max(bounds.mMax.x, vertex.pos.x), std::max(bounds.mMax.y, vertex.pos.y));
            bounds.mUVMin = ImVec2(std::min(bounds.mUVMin.x, vertex.uv.x), std::min(bounds.mUVMin.y, vertex.uv.y));
            bounds.mUVMax = ImVec2(std::max(bounds.mUVMax.x, vertex.uv.x), std::max(bounds.mUVMax.y, vertex.uv.y));
        }
        return bounds;
    }

    // what one list adds to the background list, from where it was
    struct Submitted
    {
        int mFirst = 0;
        int mLast = 0;
    };

    Submitted SubmitList(ImGuiUIBackend &backend, const UIDrawList &list, const UIFonts &fonts)
    {
        static UITextures textures;
        ImDrawList &drawn = *ImGui::GetBackgroundDrawList();
        const int first = drawn.VtxBuffer.Size;
        backend.Submit(kScreen, 1.0f, list, textures, fonts);
        return Submitted{first, drawn.VtxBuffer.Size};
    }
} // namespace

TEST_CASE("A transform moves the vertices a command drew, and a gradient colours them where they were drawn",
          "[gameui][transform]")
{
    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    UIFonts fonts;

    UIDrawList list;
    list.Reset(kScreen.width, kScreen.height);
    list.PushTransform(UITransform::Translate(400, 0) * UITransform::Rotate(std::numbers::pi_v<float> * 0.5f));
    list.Rect({0, 0, 100, 10}, kUIWhite);
    list.PopTransform();
    const Submitted turned = SubmitList(backend, list, fonts);
    REQUIRE(turned.mLast - turned.mFirst == 4);

    // a quarter turn about the origin, then 400 across: 390 to 400 across and 0 to 100 down
    const ImDrawList &drawn = *ImGui::GetBackgroundDrawList();
    const VertexBounds bounds = BoundsOf(drawn, turned.mFirst, turned.mLast);
    CHECK(bounds.mMin.x == Catch::Approx(390.0f).margin(1e-3));
    CHECK(bounds.mMax.x == Catch::Approx(400.0f).margin(1e-3));
    CHECK(bounds.mMin.y == Catch::Approx(0.0f).margin(1e-3));
    CHECK(bounds.mMax.y == Catch::Approx(100.0f).margin(1e-3));

    list.Reset(kScreen.width, kScreen.height);
    const UIGradient gradient{.mFrom = {0, 0}, .mTo = {100, 0}, .mFromColor = {1, 0, 0, 1}, .mToColor = {0, 0, 1, 1}};
    list.Rect({0, 100, 100, 10}, kUIWhite, 0.0f, gradient);
    const Submitted shaded = SubmitList(backend, list, fonts);
    REQUIRE(shaded.mLast - shaded.mFirst == 4);
    for (int i = shaded.mFirst; i < shaded.mLast; ++i)
    {
        const ImDrawVert &vertex = drawn.VtxBuffer[i];
        const ImVec4 color = ImGui::ColorConvertU32ToFloat4(vertex.col);
        CHECK(color.x == Catch::Approx(vertex.pos.x < 50.0f ? 1.0f : 0.0f).margin(0.01));
        CHECK(color.z == Catch::Approx(vertex.pos.x < 50.0f ? 0.0f : 1.0f).margin(0.01));
        CHECK(color.w == Catch::Approx(1.0f).margin(0.01));
    }
    ImGui::Render();
}

TEST_CASE("Scaled text is drawn at the size it ends up, so its glyphs stay sharp", "[gameui][transform]")
{
    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    UIFonts fonts;

    // the glyph at both sizes first, so the atlas does not grow while the quads are compared
    ImFont *font = ImGui::GetFont();
    font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, "W");
    font->CalcTextSizeA(32.0f, FLT_MAX, 0.0f, "W");

    const auto glyph = [&](float size, const UITransform &transform)
    {
        UIDrawList list;
        list.Reset(kScreen.width, kScreen.height);
        list.PushTransform(transform);
        list.Text(100, 100, "W", kUIWhite, size);
        list.PopTransform();
        const Submitted text = SubmitList(backend, list, fonts);
        REQUIRE(text.mLast - text.mFirst == 4);
        return BoundsOf(*ImGui::GetBackgroundDrawList(), text.mFirst, text.mLast);
    };

    const VertexBounds small = glyph(16.0f, {});
    const VertexBounds large = glyph(32.0f, {});
    const VertexBounds scaled = glyph(16.0f, UITransform::Scale(2, 2, {100, 100}));

    const float smallTexels = small.mUVMax.y - small.mUVMin.y;
    const float largeTexels = large.mUVMax.y - large.mUVMin.y;
    const float scaledTexels = scaled.mUVMax.y - scaled.mUVMin.y;
    CHECK(scaledTexels == Catch::Approx(largeTexels).epsilon(0.1));
    CHECK(scaledTexels > smallTexels * 1.5f);

    // and lands where the 32 unit glyph does
    CHECK(scaled.mMin.x == Catch::Approx(large.mMin.x).margin(0.5));
    CHECK(scaled.mMax.y == Catch::Approx(large.mMax.y).margin(0.5));
    ImGui::Render();
}

TEST_CASE("Text a transform moves into view is drawn though it starts outside the clip", "[gameui][transform]")
{
    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    UIFonts fonts;

    UIDrawList list;
    list.Reset(kScreen.width, kScreen.height);
    list.PushTransform(UITransform::Translate(300, 0));
    list.Text(-200, 50, "Heading", kUIWhite, 16.0f);
    list.PopTransform();
    const Submitted text = SubmitList(backend, list, fonts);
    REQUIRE(text.mLast - text.mFirst >= 4 * 7);
    CHECK(BoundsOf(*ImGui::GetBackgroundDrawList(), text.mFirst, text.mLast).mMin.x >= 99.0f);
    ImGui::Render();
}

TEST_CASE("Every shape draws through ImGui", "[gameui][shapes]")
{
    ImGuiFixture imgui;
    ImGui::NewFrame();
    ImGuiUIBackend backend;
    UIFonts fonts;

    // an arrow, which is not convex
    const UIPoint arrow[] = {{0, 10}, {30, 10}, {30, 0}, {50, 20}, {30, 40}, {30, 30}, {0, 30}};
    const std::function<void(UIDrawList &)> shapes[] = {
        [&](UIDrawList &list) { list.Line({10, 10}, {90, 40}, kUIWhite, UIStroke{.mWidth = 3}); },
        [&](UIDrawList &list) { list.Polyline(arrow, kUIWhite, UIStroke{.mWidth = 2}, true); },
        [&](UIDrawList &list) { list.Polygon(arrow, kUIWhite); },
        [&](UIDrawList &list) { list.FillCircle({100, 100}, 20, kUIWhite); },
        [&](UIDrawList &list) { list.StrokeCircle({100, 100}, 20, kUIWhite, UIStroke{.mWidth = 2}); },
        [&](UIDrawList &list) { list.Arc({100, 100}, 30, 0.0f, 2.0f, kUIWhite, UIStroke{.mWidth = 4}); },
        [&](UIDrawList &list) { list.Pie({100, 100}, 30, -1.0f, 3.0f, kUIWhite); },
        [&](UIDrawList &list) { list.StrokeRect({10, 10, 80, 40}, kUIWhite, UIStroke{.mWidth = 2}, 6.0f); },
        [&](UIDrawList &list) {
            list.StrokeCircle({100, 100}, 20, kUIWhite, UIStroke{.mWidth = 2, .mDash = 6, .mGap = 4});
        },
    };
    for (const auto &shape : shapes)
    {
        UIDrawList list;
        list.Reset(kScreen.width, kScreen.height);
        list.PushTransform(UITransform::Rotate(0.3f, {100, 100}));
        shape(list);
        list.PopTransform();
        REQUIRE_FALSE(list.Commands().empty());
        const Submitted drawn = SubmitList(backend, list, fonts);
        CHECK(drawn.mLast - drawn.mFirst >= 3);
    }
    ImGui::Render();
}
