/*
The MIT License (MIT)

Copyright (c) 2014 Marianne Gagnon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
 */

#include <algorithm>
#include <functional>
#include <cctype>
#include <locale>
#include "Dependency.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <sys/param.h>
#include "Utils.h"
#include "Settings.h"
#include "DylibBundler.h"

#include <stdlib.h>
#include <sstream>
#include <vector>

std::string stripPrefix(std::string in)
{
    return in.substr(in.rfind("/")+1);
}

// std::string& rtrim(std::string &s) {
//     s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
//     return s;
// }

static inline void rtrim_in_place(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from end (copying)
static inline std::string rtrim(std::string s) {
    rtrim_in_place(s);
    return s;
}

//the paths to search for dylibs, store it globally to parse the environment variables only once
std::vector<std::string> paths;

//initialize the dylib search paths
void initSearchPaths(){
    //Check the same paths the system would search for dylibs
    std::string searchPaths;
    char *dyldLibPath = std::getenv("DYLD_LIBRARY_PATH");
    if( dyldLibPath!=0 )
        searchPaths = dyldLibPath;
    dyldLibPath = std::getenv("DYLD_FALLBACK_FRAMEWORK_PATH");
    if (dyldLibPath != 0)
    {
        if (!searchPaths.empty() && searchPaths[ searchPaths.size()-1 ] != ':') searchPaths += ":";
        searchPaths += dyldLibPath;
    }
    dyldLibPath = std::getenv("DYLD_FALLBACK_LIBRARY_PATH");
    if (dyldLibPath!=0 )
    {
        if (!searchPaths.empty() && searchPaths[ searchPaths.size()-1 ] != ':') searchPaths += ":";
        searchPaths += dyldLibPath;
    }
    if (!searchPaths.empty())
    {
        std::stringstream ss(searchPaths);
        std::string item;
        while(std::getline(ss, item, ':'))
        {
            if (item[ item.size()-1 ] != '/') item += "/";
            paths.push_back(item);
        }
    }
}

// if some libs are missing prefixes, this will be set to true
// more stuff will then be necessary to do
bool missing_prefixes = false;

Dependency::Dependency(std::string path)
{
    char original_file_buffer[PATH_MAX];
    std::string original_file;

    if (isRpath(path))
    {
        original_file = searchFilenameInRpaths(path);
    }
    else if (not realpath(rtrim(path).c_str(), original_file_buffer))
    {
        std::cerr << "\n/!\\ WARNING : Cannot resolve path '" << path.c_str() << "'" << std::endl;
        original_file = path;
    }
    else
    {
        original_file = original_file_buffer;
    }

    // check if given path is a symlink
    if (original_file != rtrim(path))
    {
        filename = stripPrefix(original_file);
        prefix = original_file.substr(0, original_file.rfind("/")+1);
        addSymlink(path);
    }
    else
    {
        filename = stripPrefix(path);
        prefix = path.substr(0, path.rfind("/")+1);
    }
    
    //check if the lib is in a known location
    if( !prefix.empty() && prefix[ prefix.size()-1 ] != '/' ) prefix += "/";
    if( prefix.empty() || !fileExists( prefix+filename ) )
    {
        //the paths contains at least /usr/lib so if it is empty we have not initialized it
        if( paths.empty() ) initSearchPaths();
        
        //check if file is contained in one of the paths
        for( size_t i=0; i<paths.size(); ++i)
        {
            if (fileExists( paths[i]+filename ))
            {
                std::cout << " * FOUND " << filename << " in " << paths[i] << std::endl;
                prefix = paths[i];
                missing_prefixes = true; //the prefix was missing
                break;
            }
        }
    }
    
    //If the location is still unknown, ask the user for search path
    if( !Settings::isPrefixIgnored(prefix)
        && ( prefix.empty() || !fileExists( prefix+filename ) ) )
    {
        std::cerr << "\n/!\\ WARNING : Library " << filename << " has an incomplete name (location unknown)" << std::endl;
        missing_prefixes = true;
        
        paths.push_back(getUserInputDirForFile(filename));
    }
    
    //new_name  = filename.substr(0, filename.find(".")) + ".dylib";
    new_name = filename;
}

void Dependency::print()
{
    std::cout << std::endl;
    std::cout << " * " << filename.c_str() << " from " << prefix.c_str() << std::endl;
    
    const int symamount = symlinks.size();
    for(int n=0; n<symamount; n++)
        std::cout << "     symlink --> " << symlinks[n].c_str() << std::endl;;
}

std::string Dependency::getInstallPath()
{
    return Settings::destFolder() + new_name;
}
std::string Dependency::getInnerPath()
{
    return Settings::inside_lib_path() + new_name;
}


void Dependency::addSymlink(std::string s)
{
    // calling std::find on this vector is not near as slow as an extra invocation of install_name_tool
    if(std::find(symlinks.begin(), symlinks.end(), s) == symlinks.end()) symlinks.push_back(s);
}

// Compares the given Dependency with this one. If both refer to the same file,
// it returns true and merges both entries into one.
bool Dependency::mergeIfSameAs(Dependency& dep2)
{
    if(dep2.getOriginalFileName().compare(filename) == 0)
    {
        const int samount = getSymlinkAmount();
        for(int n=0; n<samount; n++) {
            dep2.addSymlink(getSymlink(n));
        }
        return true;
    }
    return false;
}

void Dependency::copyYourself()
{
    copyFile(getOriginalPath(), getInstallPath());
    
    // Fix the lib's inner name
    std::string command = std::string("install_name_tool -id ") + getInnerPath() + " " + getInstallPath();
    if( systemp( command ) != 0 )
    {
        std::cerr << "\n\nError : An error occured while trying to change identity of library " << getInstallPath() << std::endl;
        exit(1);
    }
}

void Dependency::fixFileThatDependsOnMe(std::string file_to_fix)
{
    // for main lib file
    std::string command = std::string("install_name_tool -change ") +
    getOriginalPath() + " " + getInnerPath() + " " + file_to_fix;
    
    if( systemp( command ) != 0 )
    {
        std::cerr << "\n\nError : An error occured while trying to fix dependencies of " << file_to_fix << std::endl;
        exit(1);
    }
    
    // for symlinks
    const int symamount = symlinks.size();
    for(int n=0; n<symamount; n++)
    {
        command = std::string("install_name_tool -change ") +
        symlinks[n] + " " + getInnerPath() + " " + file_to_fix;
        
        if( systemp( command ) != 0 )
        {
            std::cerr << "\n\nError : An error occured while trying to fix dependencies of " << file_to_fix << std::endl;
            exit(1);
        }
    }
    
    
    // FIXME - hackish
    if(missing_prefixes)
    {
        // for main lib file
        command = std::string("install_name_tool -change ") +
        filename + " " + getInnerPath() + " " + file_to_fix;
        
        if( systemp( command ) != 0 )
        {
            std::cerr << "\n\nError : An error occured while trying to fix dependencies of " << file_to_fix << std::endl;
            exit(1);
        }
        
        // for symlinks
        const int symamount = symlinks.size();
        for(int n=0; n<symamount; n++)
        {
            command = std::string("install_name_tool -change ") +
            symlinks[n] + " " + getInnerPath() + " " + file_to_fix;
            
            if( systemp( command ) != 0 )
            {
                std::cerr << "\n\nError : An error occured while trying to fix dependencies of " << file_to_fix << std::endl;
                exit(1);
            }
        }//next
    }// end if(missing_prefixes)
}
