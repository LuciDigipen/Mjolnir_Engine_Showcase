/**
 * @file Authoring.h
 * @author Dong Ta
 * @brief The authored-state seam: commands that change authoring documents through a backend App supplies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>
#include <editor/history/DocumentId.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    
    struct AuthoredObject
    {
        DocumentId mDocument;
        std::vector<uint64_t> mAddress;

        bool operator==(const AuthoredObject &) const = default;
    };

    
    struct AuthoredKey
    {
        AuthoredObject mObject;
        std::string mComponent;
        std::string mField;

        bool operator==(const AuthoredKey &) const = default;
    };

    
    struct AuthoredEntry
    {
        AuthoredKey mKey;
        std::optional<std::string> mValue; //encoded by the backend
    };

    struct AuthoringWrite
    {
        bool mApplied = false;

        //documents the write changed in a way Undo leaves alone, such as refs cleared into a deleted node
        std::vector<DocumentId> mMaintained;
    };

    //App's view of every authoring document, so editor code never sees a Prefab type
    class IAuthoringBackend
    {
    public:
        virtual ~IAuthoringBackend() = default;

        
        virtual std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) = 0;

        //validates the whole batch across its documents, atomic
        virtual AuthoringWrite Write(std::span<const AuthoredEntry> entries) = 0;
    };

    
    std::unique_ptr<ICommand> MakeAuthoredCommand(std::string name, std::vector<AuthoredEntry> changes,
        std::vector<AuthoredEntry> maintenance = {});
}
