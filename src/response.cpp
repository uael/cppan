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

#include "response.h"

#include "config.h"
#include "file_lock.h"
#include "log.h"
#include "project.h"

ResponseData rd;

void ResponseData::init(Config *config, const String &host, const path &root_dir)
{
    if (executed || initialized)
        return;

    this->host = host;
    this->root_dir = root_dir;

    // add default (current, root) config
    packages[Package()].config = config;

    initialized = true;
}

void ResponseData::download_dependencies(const Packages &deps)
{
    if (executed || !initialized)
        return;

    if (deps.empty())
        return;

    // prepare request
    for (auto &d : deps)
    {
        ptree version;
        version.put("version", d.second.version.toString());
        request.put_child(ptree::path_type(d.second.ppath.toString(), '|'), version);
    }

    LOG_NO_NEWLINE("Requesting dependency list... ");
    dependency_tree = url_post(host + "/api/find_dependencies", request);

    // read deps urls, download them, unpack
    int api = 0;
    if (dependency_tree.find("api") != dependency_tree.not_found())
        api = dependency_tree.get<int>("api");

    auto e = dependency_tree.find("error");
    if (e != dependency_tree.not_found())
        throw std::runtime_error(e->second.get_value<String>());

    if (api == 0)
        throw std::runtime_error("Api version is missing in the response");
    if (api != 1)
        throw std::runtime_error("Bad api version");

    data_url = "data";
    if (dependency_tree.find("data_dir") != dependency_tree.not_found())
        data_url = dependency_tree.get<String>("data_dir");

    // dependencies were received without error
    LOG("Ok");

    extractDependencies();
    download_and_unpack();
    post_download();
    write_index();

    // add default (current, root) config
    packages[Package()].dependencies = deps;
    for (auto &dd : download_dependencies_)
    {
        if (!dd.second.flags[pfDirectDependency])
            continue;
        auto &deps2 = packages[Package()].dependencies;
        auto i = deps2.find(dd.second.ppath.toString());
        if (i == deps2.end())
        {
            // check if we chosen a root project match all subprojects
            Packages to_add;
            std::set<String> to_remove;
            for (auto &root_dep : deps2)
            {
                for (auto &child_dep : download_dependencies_)
                {
                    if (root_dep.second.ppath.is_root_of(child_dep.second.ppath))
                    {
                        to_add.insert({ child_dep.second.ppath.toString(), child_dep.second });
                        to_remove.insert(root_dep.second.ppath.toString());
                    }
                }
            }
            if (to_add.empty())
                throw std::runtime_error("cannot match dependency");
            for (auto &r : to_remove)
                deps2.erase(r);
            for (auto &a : to_add)
                deps2.insert(a);
            continue;
        }
        auto &d = i->second;
        d.version = dd.second.version;
        d.flags |= dd.second.flags;
        d.createNames();
    }

    // last in functions
    executed = true;
}

void ResponseData::extractDependencies()
{
    LOG_NO_NEWLINE("Reading package specs... ");
    auto &remote_packages = dependency_tree.get_child("packages");
    for (auto &v : remote_packages)
    {
        auto id = v.second.get<int>("id");

        DownloadDependency d;
        d.ppath = v.first;
        d.version = v.second.get<String>("version");
        d.flags = decltype(d.flags)(v.second.get<uint64_t>("flags"));
        d.md5 = v.second.get<String>("md5");
        d.createNames();
        dep_ids[d] = id;

        if (fs::exists(d.getDirSrc()))
        {
            auto old = fs::current_path();
            try
            {
                auto p = config_store.insert(std::make_unique<Config>(d.getDirSrc()));
                packages[d].config = p.first->get();
            }
            catch (std::exception &)
            {
                // something wrong, remove the whole dir to re-download it
                fs::current_path(old);
                fs::remove_all(d.getDirSrc());
            }
        }

        if (v.second.find(DEPENDENCIES_NODE) != v.second.not_found())
        {
            std::set<int> idx;
            for (auto &tree_dep : v.second.get_child(DEPENDENCIES_NODE))
                idx.insert(tree_dep.second.get_value<int>());
            d.setDependencyIds(idx);
        }

        d.map_ptr = &download_dependencies_;
        download_dependencies_[id] = d;
    }
    LOG("Ok");
}

void ResponseData::download_and_unpack()
{
    for (auto &dd : download_dependencies_)
    {
        auto &d = dd.second;
        auto version_dir = d.getDirSrc();
        auto md5file = d.getStampFilename();

        // store md5 of archive
        bool must_download = false;
        {
            std::ifstream ifile(md5file.string());
            String file_md5;
            if (ifile)
            {
                ifile >> file_md5;
                ifile.close();
            }
            if (file_md5 != d.md5 || d.md5.empty() || file_md5.empty())
                must_download = true;
        }

        if (fs::exists(version_dir) && !must_download)
            continue;

        auto add_config = [this, &d]()
        {
            auto p = config_store.insert(std::make_unique<Config>(d.getDirSrc()));
            packages[d].config = p.first->get();
            packages[d].config->downloaded = true;
            return packages[d].config;
        };

        // lock, so only one cppan process at the time could download the project
        ScopedFileLock lck(md5file, std::defer_lock);
        if (!lck.try_lock())
        {
            // wait & continue
            ScopedFileLock lck2(md5file);
            add_config();
            continue;
        }

        // remove existing version
        cleanPackages(d.target_name);

        auto fs_path = ProjectPath(d.ppath).toFileSystemPath().string();
        std::replace(fs_path.begin(), fs_path.end(), '\\', '/');
        String package_url = host + "/" + data_url + "/" + fs_path + "/" + d.version.toString() + ".tar.gz";
        path fn = version_dir.string() + ".tar.gz";

        String dl_md5;
        DownloadData ddata;
        ddata.url = package_url;
        ddata.fn = fn;
        ddata.dl_md5 = &dl_md5;
        LOG_NO_NEWLINE("Downloading: " << d.target_name << "... ");
        download_file(ddata);
        downloads++;

        if (dl_md5 != d.md5)
        {
            LOG("Fail");
            throw std::runtime_error("md5 does not match for package '" + d.ppath.toString() + "'");
        }
        LOG("Ok");

        write_file(md5file, d.md5);

        LOG_NO_NEWLINE("Unpacking  : " << d.target_name << "... ");
        Files files;
        try
        {
            files = unpack_file(fn, version_dir);
        }
        catch (...)
        {
            fs::remove_all(version_dir);
            throw;
        }
        fs::remove(fn);
        LOG("Ok");

        // re-read in any case
        // no need to remove old config, let it die with program
        auto c = add_config();

        // move all files under unpack dir
        auto ud = c->getDefaultProject().unpack_directory;
        if (!ud.empty())
        {
            ud = version_dir / ud;
            if (fs::exists(ud))
                throw std::runtime_error("Cannot create unpack_directory '" + ud.string() + "' because fs object with the same name alreasy exists");
            fs::create_directories(ud);
            for (auto &f : boost::make_iterator_range(fs::directory_iterator(version_dir), {}))
            {
                if (f == ud || f.path().filename() == CPPAN_FILENAME)
                    continue;
                if (fs::is_directory(f))
                {
                    copy_dir(f, ud / f.path().filename());
                    fs::remove_all(f);
                }
                else if (fs::is_regular_file(f))
                {
                    fs::copy_file(f, ud / f.path().filename());
                    fs::remove(f);
                }
            }
        }
    }
}

void ResponseData::post_download()
{
    for (auto &cc : packages)
        prepare_config(cc);
}

void ResponseData::prepare_config(PackageConfigs::value_type &cc)
{
    auto &p = cc.first;
    auto &c = cc.second.config;
    auto &dependencies = cc.second.dependencies;
    c->is_dependency = true;
    c->pkg = p;
    auto &project = c->getDefaultProject();
    project.pkg = cc.first;

    // prepare deps: extract real deps flags from configs
    for (auto &dep : download_dependencies_[dep_ids[p]].getDirectDependencies())
    {
        auto d = dep.second;
        auto i = project.dependencies.find(d.ppath.toString());
        if (i == project.dependencies.end())
            throw std::runtime_error("dependency '" + d.ppath.toString() + "' is not found");
        d.flags[pfIncludeDirectories] = i->second.flags[pfIncludeDirectories];
        i->second.version = d.version;
        i->second.flags = d.flags;
        dependencies.emplace(d.ppath.toString(), d);
    }

    // turn off this feature atm
    // #include <pvt/cppan/demo/zlib.h>
    // #include <org/boost/filesystem.hpp>
    // #include <org/qt/widgets.h>
    /*if (!p.empty())
    {
        // create include directory structure
        auto dir = directories.storage_dir_lnk / "src" / p.version.toPath() / p.ppath.toPath();
        auto src = p.getDirSrc();
#ifndef _WIN32
        // for non windows systems create symlink
        // uncomment this when libs will provide all includes in include dir
        fs::create_directory_symlink(src / "include", dir);
#else
        // windows requires to be admin or to be added to create_symlink policy
        // so just copy includes
        if (!fs::exists(dir) || c->downloaded)
        {
            if (c->downloaded)
                fs::remove_all(dir);
            fs::create_directories(dir);
            for (auto &i : project.include_directories.public_)
            {
                if (!fs::exists(src / i))
                    continue;
                for (auto &f : boost::make_iterator_range(fs::directory_iterator(src / i), {}))
                {
                    if (fs::is_directory(f))
                        copy_dir(f, dir / f.path().filename());
                    else if (fs::is_regular_file(f))
                        fs::copy_file(f, dir / f.path().filename());
                }
            }
        }
#endif
    }*/

    c->post_download();
}

ResponseData::PackageConfig &ResponseData::operator[](const Package &p)
{
    return packages[p];
}

const ResponseData::PackageConfig &ResponseData::operator[](const Package &p) const
{
    auto i = packages.find(p);
    if (i == packages.end())
        throw std::runtime_error("Package not found: " + p.getTargetName());
    return i->second;
}

ResponseData::iterator ResponseData::begin()
{
    auto i = packages.find(Package());
    if (i != packages.end())
        return ++i;
    return packages.begin();
}

ResponseData::iterator ResponseData::end()
{
    return packages.end();
}

ResponseData::const_iterator ResponseData::begin() const
{
    auto i = packages.find(Package());
    if (i != packages.end())
        return ++i;
    return packages.begin();
}

ResponseData::const_iterator ResponseData::end() const
{
    return packages.end();
}

void ResponseData::write_index() const
{
    auto renew_index = [this](const auto &dir, auto func)
    {
        PackageIndex pkgs = readPackagesIndex(dir);
        for (auto &cc : *this)
            pkgs[cc.first.target_name] = (cc.first.*func)();
        writePackagesIndex(dir, pkgs);
    };

    renew_index(directories.storage_dir_src, &Package::getDirSrc);
    renew_index(directories.storage_dir_obj, &Package::getDirObj);
}
