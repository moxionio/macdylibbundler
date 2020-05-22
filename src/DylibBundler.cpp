#include "DylibBundler.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <utility>

#ifdef __linux
#include <linux/limits.h>
#endif
#ifndef __clang__
#include <sys/types.h>
#endif

#include "Dependency.h"
#include "Settings.h"
#include "Utils.h"

std::vector<Dependency> deps;
std::map<std::string, std::vector<Dependency>> deps_per_file;
std::map<std::string, bool> deps_collected;
std::set<std::string> frameworks;
std::set<std::string> rpaths;
std::map<std::string, bool> rpaths_collected;
bool qt_plugins_called = false;

void addDependency(const std::string& path, const std::string& dependent_file)
{
    Dependency dependency(path, dependent_file);

    // check if this library was already added to |deps| to avoid duplicates
    bool in_deps = false;
    for (auto& dep : deps) {
        if (dependency.MergeIfIdentical(dep))
            in_deps = true;
    }

    // check if this library was already added to |deps_per_file[dependent_file]| to avoid duplicates
    bool in_deps_per_file = false;
    for (auto& dep : deps_per_file[dependent_file]) {
        if (dependency.MergeIfIdentical(dep))
            in_deps_per_file = true;
    }

    // check if this library is in /usr/lib, /System/Library, or in ignored list
    if (!Settings::isPrefixBundled(dependency.Prefix()))
        return;

    if (!in_deps && dependency.IsFramework())
        frameworks.insert(dependency.OriginalPath());
    if (!in_deps)
        deps.push_back(dependency);
    if (!in_deps_per_file)
        deps_per_file[dependent_file].push_back(dependency);
}

void collectDependenciesRpaths(const std::string& dependent_file)
{
    if (deps_collected.find(dependent_file) != deps_collected.end() && Settings::fileHasRpath(dependent_file))
        return;

    std::map<std::string,std::string> cmds_values;
    std::string dylib = "LC_LOAD_DYLIB";
    std::string rpath = "LC_RPATH";
    cmds_values[dylib] = "name";
    cmds_values[rpath] = "path";
    std::map<std::string,std::vector<std::string>> cmds_results;

    parseLoadCommands(dependent_file, cmds_values, cmds_results);

    if (rpaths_collected.find(dependent_file) == rpaths_collected.end()) {
        auto rpath_results = cmds_results[rpath];
        for (const auto& rpath_result : rpath_results) {
            rpaths.insert(rpath_result);
            Settings::addRpathForFile(dependent_file, rpath_result);
            if (Settings::verboseOutput())
                std::cout << "  rpath: " << rpath_result << std::endl;
        }
        rpaths_collected[dependent_file] = true;
    }

    if (deps_collected.find(dependent_file) == deps_collected.end()) {
        auto dylib_results = cmds_results[dylib];
        for (const auto& dylib_result : dylib_results) {
            // skip system/ignored prefixes
            if (Settings::isPrefixBundled(dylib_result))
                addDependency(dylib_result, dependent_file);
        }
        deps_collected[dependent_file] = true;
    }
}

void collectSubDependencies()
{
    size_t dep_counter = deps.size();
    if (Settings::verboseOutput()) {
        std::cout << "(pre sub) # OF FILES: " << Settings::filesToFixCount() << std::endl;
        std::cout << "(pre sub) # OF DEPS: " << deps.size() << std::endl;
    }

    size_t deps_size = deps.size();
    while (true) {
        deps_size = deps.size();
        for (size_t n=0; n<deps_size; ++n) {
            std::string original_path = deps[n].OriginalPath();
            if (Settings::verboseOutput())
                std::cout << "  (collect sub deps) original path: " << original_path << std::endl;
            if (isRpath(original_path))
                original_path = searchFilenameInRpaths(original_path);
            collectDependenciesRpaths(original_path);
        }
        // if no more dependencies were added on this iteration, stop searching
        if (deps.size() == deps_size)
            break;
    }

    if (Settings::verboseOutput()) {
        std::cout << "(post sub) # OF FILES: " << Settings::filesToFixCount() << std::endl;
        std::cout << "(post sub) # OF DEPS: " << deps.size() << std::endl;
    }
    if (Settings::bundleLibs() && Settings::bundleFrameworks()) {
        if (!qt_plugins_called || (deps.size() != dep_counter))
            bundleQtPlugins();
    }
}

void changeLibPathsOnFile(const std::string& file_to_fix)
{
    if (deps_collected.find(file_to_fix) == deps_collected.end() || rpaths_collected.find(file_to_fix) == rpaths_collected.end())
        collectDependenciesRpaths(file_to_fix);

    std::cout << "* Fixing dependencies on " << file_to_fix << "\n";

    std::vector<Dependency> dependencies = deps_per_file[file_to_fix];
    for (auto& dependency : dependencies)
        dependency.FixDependentFile(file_to_fix);
}

void fixRpathsOnFile(const std::string& original_file, const std::string& file_to_fix)
{
    std::vector<std::string> rpaths_to_fix;
    if (!Settings::fileHasRpath(original_file))
        return;

    rpaths_to_fix = Settings::getRpathsForFile(original_file);
    for (const auto& rpath_to_fix : rpaths_to_fix) {
        std::string command = std::string("install_name_tool -rpath ") + rpath_to_fix + " " + Settings::insideLibPath() + " " + file_to_fix;
        if (systemp(command) != 0) {
            std::cerr << "\n\n/!\\ ERROR: An error occured while trying to fix rpath " << rpath_to_fix << " of " << file_to_fix << std::endl;
            exit(1);
        }
    }
}

void bundleDependencies()
{
    for (const auto& dep : deps)
        dep.Print();
    std::cout << "\n";
    if (Settings::verboseOutput()) {
        std::cout << "rpaths:" << std::endl;
        for (const auto& rpath : rpaths)
            std::cout << "* " << rpath << std::endl;
    }

    // copy & fix up dependencies
    if (Settings::bundleLibs()) {
        createDestDir();
        for (const auto& dep : deps) {
            dep.CopyToBundle();
            changeLibPathsOnFile(dep.InstallPath());
            fixRpathsOnFile(dep.OriginalPath(), dep.InstallPath());
        }
    }
    // fix up selected files
    const auto files = Settings::filesToFix();
    for (const auto& file : files) {
        changeLibPathsOnFile(file);
        fixRpathsOnFile(file, file);
    }
}

void bundleQtPlugins()
{
    bool qtCoreFound = false;
    bool qtGuiFound = false;
    bool qtNetworkFound = false;
    bool qtSqlFound = false;
    bool qtSvgFound = false;
    bool qtMultimediaFound = false;
    bool qt3dRenderFound = false;
    bool qt3dQuickRenderFound = false;
    bool qtPositioningFound = false;
    bool qtLocationFound = false;
    bool qtTextToSpeechFound = false;
    bool qtWebViewFound = false;
    std::string original_file;

    for (const auto& framework : frameworks) {
        if (framework.find("QtCore") != std::string::npos) {
            qtCoreFound = true;
            original_file = framework;
        }
        if (framework.find("QtGui") != std::string::npos)
            qtGuiFound = true;
        if (framework.find("QtNetwork") != std::string::npos)
            qtNetworkFound = true;
        if (framework.find("QtSql") != std::string::npos)
            qtSqlFound = true;
        if (framework.find("QtSvg") != std::string::npos)
            qtSvgFound = true;
        if (framework.find("QtMultimedia") != std::string::npos)
            qtMultimediaFound = true;
        if (framework.find("Qt3DRender") != std::string::npos)
            qt3dRenderFound = true;
        if (framework.find("Qt3DQuickRender") != std::string::npos)
            qt3dQuickRenderFound = true;
        if (framework.find("QtPositioning") != std::string::npos)
            qtPositioningFound = true;
        if (framework.find("QtLocation") != std::string::npos)
            qtLocationFound = true;
        if (framework.find("TextToSpeech") != std::string::npos)
            qtTextToSpeechFound = true;
        if (framework.find("WebView") != std::string::npos)
            qtWebViewFound = true;
    }

    if (!qtCoreFound)
        return;
    if (!qt_plugins_called)
        createQtConf(Settings::resourcesFolder());
    qt_plugins_called = true;

    const auto fixupPlugin = [original_file](const std::string& plugin) {
        std::string dest = Settings::pluginsFolder();
        std::string framework_root = getFrameworkRoot(original_file);
        std::string prefix = filePrefix(framework_root);
        std::string qt_prefix = filePrefix(prefix.substr(0, prefix.size()-1));
        std::string qt_plugins_prefix = qt_prefix + "plugins/";
        if (fileExists(qt_plugins_prefix + plugin) && !fileExists(dest + plugin)) {
            mkdir(dest + plugin);
            copyFile(qt_plugins_prefix + plugin, dest);
            std::vector<std::string> files = lsDir(dest + plugin+"/");
            for (const auto& file : files) {
                Settings::addFileToFix(dest + plugin+"/"+file);
                collectDependenciesRpaths(dest + plugin+"/"+file);
                changeId(dest + plugin+"/"+file, "@rpath/" + plugin+"/"+file);
            }
        }
    };

    std::string framework_root = getFrameworkRoot(original_file);
    std::string prefix = filePrefix(framework_root);
    std::string qt_prefix = filePrefix(prefix.substr(0, prefix.size()-1));
    std::string qt_plugins_prefix = qt_prefix + "plugins/";
    std::string dest = Settings::pluginsFolder();

    if (!fileExists(dest + "platforms")) {
        mkdir(dest + "platforms");
        copyFile(qt_plugins_prefix + "platforms/libqcocoa.dylib", dest + "platforms");
        Settings::addFileToFix(dest + "platforms/libqcocoa.dylib");
        collectDependenciesRpaths(dest + "platforms/libqcocoa.dylib");
        changeId(dest + "platforms/libqcocoa.dylib", "@rpath/platforms/libqcocoa.dylib");
    }

    fixupPlugin("printsupport");
    fixupPlugin("styles");
    fixupPlugin("imageformats");
    fixupPlugin("iconengines");
    if (!qtSvgFound)
        systemp(std::string("rm -f ") + dest + "imageformats/libqsvg.dylib");
    if (qtGuiFound) {
        fixupPlugin("platforminputcontexts");
        fixupPlugin("virtualkeyboard");
    }
    if (qtNetworkFound)
        fixupPlugin("bearer");
    if (qtSqlFound)
        fixupPlugin("sqldrivers");
    if (qtMultimediaFound) {
        fixupPlugin("mediaservice");
        fixupPlugin("audio");
    }
    if (qt3dRenderFound) {
        fixupPlugin("sceneparsers");
        fixupPlugin("geometryloaders");
    }
    if (qt3dQuickRenderFound)
        fixupPlugin("renderplugins");
    if (qtPositioningFound)
        fixupPlugin("position");
    if (qtLocationFound)
        fixupPlugin("geoservices");
    if (qtTextToSpeechFound)
        fixupPlugin("texttospeech");
    if (qtWebViewFound)
        fixupPlugin("webview");

    collectSubDependencies();
}
