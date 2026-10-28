// SPDX-FileCopyrightText: 2014-2021 Josh Blum (SoapySDR Modules API)
// SPDX-FileCopyrightText: 2026 Chris YO3TCO / Nexus-BS Project
// SPDX-License-Identifier: BSL-1.0
//
// SoapySDR module loader for RTEMS: there are no loadable modules in the single static image.
// Device drivers (the native Pluto AD9363 driver) register themselves through static
// SoapySDR::Registry objects linked into the image; this file keeps the Modules API and the
// helper functions the rest of the library calls, without dlopen/glob.

#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Version.hpp>
#include <map>
#include <mutex>
#include <string>
#include <vector>

static std::recursive_mutex &getModuleMutex(void)
{
    static std::recursive_mutex mutex;
    return mutex;
}

std::string getEnvImpl(const char *) { return ""; }

std::string SoapySDR::getRootPath(void) { return "/"; }

std::vector<std::string> SoapySDR::listSearchPaths(void) { return {}; }

std::vector<std::string> SoapySDR::listModules(void) { return {}; }

std::vector<std::string> SoapySDR::listModules(const std::string &) { return {}; }

std::map<std::string, void *> &getModuleHandles(void)
{
    static std::map<std::string, void *> handles;
    return handles;
}

std::string &getModuleLoading(void)
{
    static std::string moduleLoading;
    return moduleLoading;
}

std::map<std::string, SoapySDR::Kwargs> &getLoaderResults(void)
{
    static std::map<std::string, SoapySDR::Kwargs> results;
    return results;
}

std::map<std::string, std::string> &getModuleVersions(void)
{
    static std::map<std::string, std::string> versions;
    return versions;
}

SoapySDR::ModuleVersion::ModuleVersion(const std::string &version)
{
    getModuleVersions()[getModuleLoading()] = version;
}

std::string SoapySDR::loadModule(const std::string &path)
{
    return path + ": loadable modules are not supported on RTEMS (drivers are linked in)";
}

SoapySDR::Kwargs SoapySDR::getLoaderResult(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(getModuleMutex());
    auto it = getLoaderResults().find(path);
    return it == getLoaderResults().end() ? SoapySDR::Kwargs() : it->second;
}

std::string SoapySDR::getModuleVersion(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(getModuleMutex());
    auto it = getModuleVersions().find(path);
    return it == getModuleVersions().end() ? "" : it->second;
}

std::string SoapySDR::unloadModule(const std::string &path) { return path + " never loaded"; }

void lateLoadNullDevice(void);

void automaticLoadModules(void)
{
    std::lock_guard<std::recursive_mutex> lock(getModuleMutex());
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    lateLoadNullDevice();
}

void SoapySDR::loadModules(void)
{
    std::lock_guard<std::recursive_mutex> lock(getModuleMutex());
    lateLoadNullDevice();
}

void SoapySDR::unloadModules(void) {}

SoapySDR::ModuleManager::ModuleManager(const bool load)
{
    if (load) SoapySDR::loadModules();
}

SoapySDR::ModuleManager::~ModuleManager(void) {}
