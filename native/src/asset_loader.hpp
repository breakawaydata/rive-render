/*
 * Copyright 2025 BreakAway Data
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <fstream>
#include <map>
#include <string>

#include "rive/file_asset_loader.hpp"
#include "rive/assets/file_asset.hpp"
#include "rive/assets/image_asset.hpp"
#include "rive/assets/font_asset.hpp"
#include "rive/factory.hpp"
#include "rive/simple_array.hpp"

class MappedAssetLoader : public rive::FileAssetLoader
{
public:
    // assetName (uniqueFilename or uniqueName) -> local file path
    std::map<std::string, std::string> imagePaths;
    std::map<std::string, std::string> fontPaths;

    bool loadContents(rive::FileAsset& asset,
                      rive::Span<const uint8_t> inBandBytes,
                      rive::Factory* factory) override
    {
        std::string filename = asset.uniqueFilename();
        std::string name = asset.uniqueName();

        // Check if we have a mapped file for this referenced asset
        if (asset.is<rive::ImageAsset>())
        {
            auto it = imagePaths.find(filename);
            if (it == imagePaths.end())
                it = imagePaths.find(name);
            if (it != imagePaths.end())
            {
                auto bytes = readFile(it->second);
                if (!bytes.empty())
                {
                    rive::SimpleArray<uint8_t> arr(bytes.data(), bytes.size());
                    return asset.as<rive::ImageAsset>()->decode(arr, factory);
                }
            }
        }

        if (asset.is<rive::FontAsset>())
        {
            auto it = fontPaths.find(filename);
            if (it == fontPaths.end())
                it = fontPaths.find(name);
            if (it != fontPaths.end())
            {
                auto bytes = readFile(it->second);
                if (!bytes.empty())
                {
                    rive::SimpleArray<uint8_t> arr(bytes.data(), bytes.size());
                    return asset.as<rive::FontAsset>()->decode(arr, factory);
                }
            }
        }

        // For embedded assets with in-band bytes, decode them here.
        // When a custom FileAssetLoader is provided, the runtime delegates
        // all loading to it — including embedded assets.
        if (inBandBytes.size() > 0)
        {
            rive::SimpleArray<uint8_t> arr(inBandBytes.data(),
                                           inBandBytes.size());
            return asset.decode(arr, factory);
        }

        return false;
    }

private:
    static std::vector<uint8_t> readFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return {};
        auto size = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(size);
        f.read(reinterpret_cast<char*>(bytes.data()), size);
        return bytes;
    }
};
