/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "common.h"
#include "dependency.h"
#include "property_tree.h"

struct Config;
struct Directories;
class Executor;
class SqliteDatabase;

struct ResponseData
{
public:
    struct PackageConfig
    {
        Config *config;
        Packages dependencies;
    };
    using PackageConfigs = std::map<Package, PackageConfig>;

    using iterator = PackageConfigs::iterator;
    using const_iterator = PackageConfigs::const_iterator;

public:
    void init(Config *config, const String &host, const path &root_dir);
    void download_dependencies(const Packages &d);
    Config *add_config(std::unique_ptr<Config> &&config, bool created);
    Config *add_config(const Package &p);

    bool rebuild_configs() { return has_downloads() || deps_changed; }
    bool has_downloads() const { return downloads > 0; }

    PackageConfig &operator[](const Package &p);
    const PackageConfig &operator[](const Package &p) const;

    iterator begin();
    iterator end();

    const_iterator begin() const;
    const_iterator end() const;

private:
    ptree request;
    ptree dependency_tree;
    DownloadDependencies download_dependencies_;
    std::map<Package, ProjectVersionId> dep_ids;
    String host;
    String data_url{"data"};
    path root_dir;
    bool executed = false;
    bool initialized = false;
    int downloads = 0;
    bool deps_changed = false;
    PackageConfigs packages;
    std::set<std::unique_ptr<Config>> config_store;
    bool query_local_db = true;
    std::unique_ptr<Executor> executor;

    void getDependenciesFromRemote(const Packages &deps);
    void getDependenciesFromDb(const Packages &deps);
    void extractDependencies();
    void download_and_unpack();
    void post_download();
    void prepare_config(PackageConfigs::value_type &cc);
    void write_index() const;
    void read_config(const DownloadDependency &d);
    Executor &getExecutor();
};

extern ResponseData rd;
