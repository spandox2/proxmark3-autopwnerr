//-----------------------------------------------------------------------------
// Copyright (C) 2013 m h swende <martin at swende.se>
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Some lua scripting glue to proxmark core.
//-----------------------------------------------------------------------------
// 2020, added Python support (@iceman100)


#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PYTHON
//#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <wchar.h>
#endif


#include "cmdparser.h"    // command_t
#include "scripting.h"
#include "comms.h"
#include "cmdscript.h"
#include "cmdhfmf.h"
#include "pm3_binlib.h"
#include "pm3_bitlib.h"
#include "lualib.h"
#include "lauxlib.h"
#include "proxmark3.h"
#include "ui.h"
#include "fileutils.h"

typedef enum {
    PM3_LUA,
    PM3_CMD,
#ifdef HAVE_PYTHON    
    PM3_PY
#endif    
} pm3_scriptfile_t;

static int CmdHelp(const char *Cmd);

#ifdef HAVE_PYTHON

#define PYTHON_LIBRARIES_WILDCARD  "?.py"

static int split(char *str, char **arr) {
    int begin_index = 0;
    int word_cnt = 0;

    while (1) {
        while (isspace(str[begin_index])) {
            ++begin_index;
        }
        if (str[begin_index] == '\0') {
            break;
        }
        int end_index = begin_index;
        while (str[end_index] && !isspace(str[end_index])) {
            ++end_index;
        }
        int len = end_index - begin_index;
        char *tmp = calloc(len + 1, sizeof(char));
        memcpy(tmp, &str[begin_index], len);
        arr[word_cnt++] = tmp;
        begin_index = end_index;
    }
    return word_cnt;
}

static void set_python_path(char *path) {
    PyObject *syspath = PySys_GetObject("path");
    if (syspath == 0) {
        PrintAndLogEx(WARNING, "Python failed to getobject");
    }
    
    PyObject *pName = PyUnicode_FromString(path);
    if (PyList_Insert(syspath, 0, pName)) {
        PrintAndLogEx(WARNING, "Error inserting extra path into sys.path list");
    }
    
    if (PySys_SetObject("path", syspath)) {
        PrintAndLogEx(WARNING,"Error setting sys.path object");
    }
}

static void set_python_paths(void) {
    //--add to the LUA_PATH (package.path in lua)
    // so we can load scripts from various places:
    const char *exec_path = get_my_executable_directory();
    if (exec_path != NULL) {
        // from the ./luascripts/ directory
        char scripts_path[strlen(exec_path) + strlen(PYTHON_SCRIPTS_SUBDIR) + strlen(PYTHON_LIBRARIES_WILDCARD) + 1];
        strcpy(scripts_path, exec_path);
        strcat(scripts_path, PYTHON_SCRIPTS_SUBDIR);
//        strcat(scripts_path, PYTHON_LIBRARIES_WILDCARD);
        set_python_path(scripts_path);
    }

    const char *user_path = get_my_user_directory();
    if (user_path != NULL) {
        // from the $HOME/.proxmark3/luascripts/ directory
        char scripts_path[strlen(user_path) + strlen(PM3_USER_DIRECTORY) + strlen(PYTHON_SCRIPTS_SUBDIR) + strlen(PYTHON_LIBRARIES_WILDCARD) + 1];
        strcpy(scripts_path, user_path);
        strcat(scripts_path, PM3_USER_DIRECTORY);
        strcat(scripts_path, PYTHON_SCRIPTS_SUBDIR);
//        strcat(scripts_path, PYTHON_LIBRARIES_WILDCARD);
        set_python_path(scripts_path);

    }

    if (exec_path != NULL) {
        // from the $PREFIX/share/proxmark3/luascripts/ directory
        char scripts_path[strlen(exec_path) + strlen(PM3_SHARE_RELPATH) + strlen(PYTHON_SCRIPTS_SUBDIR) + strlen(PYTHON_LIBRARIES_WILDCARD) + 1];
        strcpy(scripts_path, exec_path);
        strcat(scripts_path, PM3_SHARE_RELPATH);
        strcat(scripts_path, PYTHON_SCRIPTS_SUBDIR);
//        strcat(scripts_path, PYTHON_LIBRARIES_WILDCARD);
        set_python_path(scripts_path);
    }
}
#endif

/**
* Generate a sorted list of available commands, what it does is
* generate a file listing of the script-directory for files
* ending with .lua
*/
static int CmdScriptList(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    int ret = searchAndList(LUA_SCRIPTS_SUBDIR, ".lua");
    if (ret != PM3_SUCCESS)
        return ret;
    
    ret = searchAndList(CMD_SCRIPTS_SUBDIR, ".cmd");
    if (ret != PM3_SUCCESS)
        return ret;
   
    return searchAndList(PYTHON_SCRIPTS_SUBDIR, ".py");
}

/**
 * @brief CmdScriptRun - executes a script file.
 * @param argc
 * @param argv
 * @return
 */
static int CmdScriptRun(const char *Cmd) {

    char preferredName[128] = {0};
    char arguments[256] = {0};

    int name_len = 0;
    int arg_len = 0;
    static uint8_t luascriptfile_idx = 0;
    sscanf(Cmd, "%127s%n %255[^\n\r]%n", preferredName, &name_len, arguments, &arg_len);
    
    char *extension_chk;
    extension_chk = str_dup(preferredName);
    str_lower(extension_chk);
    pm3_scriptfile_t ext = PM3_LUA;
    
    if (str_endswith(extension_chk, ".cmd"))  {
        ext = PM3_CMD;
    }
    
#ifdef HAVE_PYTHON
    if (str_endswith(extension_chk, ".py")) {
        ext = PM3_PY;
    }
#endif
        
    char *script_path = NULL;
    if ((ext == PM3_LUA) && (searchFile(&script_path, LUA_SCRIPTS_SUBDIR, preferredName, ".lua", true) == PM3_SUCCESS)) {
        int error;
        if (luascriptfile_idx == MAX_NESTED_LUASCRIPT) {
            PrintAndLogEx(ERR, "too many nested scripts, skipping %s\n", script_path);
            free(script_path);
            return PM3_EMALLOC;
        }
        PrintAndLogEx(SUCCESS, "executing lua " _YELLOW_("%s"), script_path);
        PrintAndLogEx(SUCCESS, "args " _YELLOW_("'%s'"), arguments);

        luascriptfile_idx++;

        // create new Lua state
        lua_State *lua_state;
        lua_state = luaL_newstate();

        // load Lua libraries
        luaL_openlibs(lua_state);

        //Sets the pm3 core libraries, that go a bit 'under the hood'
        set_pm3_libraries(lua_state);

        //Add the 'bin' library
        set_bin_library(lua_state);

        //Add the 'bit' library
        set_bit_library(lua_state);

        error = luaL_loadfile(lua_state, script_path);
        free(script_path);
        if (!error) {
            lua_pushstring(lua_state, arguments);
            lua_setglobal(lua_state, "args");

            //Call it with 0 arguments
            error = lua_pcall(lua_state, 0, LUA_MULTRET, 0); // once again, returns non-0 on error,
        }
        if (error) { // if non-0, then an error
            // the top of the stack should be the error string
            if (!lua_isstring(lua_state, lua_gettop(lua_state)))
                PrintAndLogEx(FAILED, "error - but no error (?!)");

            // get the top of the stack as the error and pop it off
            const char *str = lua_tostring(lua_state, lua_gettop(lua_state));
            lua_pop(lua_state, 1);
            puts(str);
        }

        //luaL_dofile(lua_state, buf);
        // close the Lua state
        lua_close(lua_state);
        luascriptfile_idx--;
        PrintAndLogEx(SUCCESS, "\nfinished " _YELLOW_("%s"), preferredName);
        return PM3_SUCCESS;
    }

    if ((ext == PM3_CMD) && (searchFile(&script_path, CMD_SCRIPTS_SUBDIR, preferredName, ".cmd", true) == PM3_SUCCESS)) {

        PrintAndLogEx(SUCCESS, "executing Cmd " _YELLOW_("%s"), script_path);
        PrintAndLogEx(SUCCESS, "args " _YELLOW_("'%s'"), arguments);

        int ret = push_cmdscriptfile(script_path, true);
        if (ret != PM3_SUCCESS)
            PrintAndLogEx(ERR, "could not open " _YELLOW_("%s") "...", script_path);
        free(script_path);
        return ret;
    }
    
    /*    
    For apt (Ubuntu, Debian...):
        sudo apt-get install python3-dev  # for python3.x installs

    For yum (CentOS, RHEL...):
        sudo yum install python3-devel   # for python3.x installs

    For dnf (Fedora...):
        sudo dnf install python3-devel  # for python3.x installs

    For zypper (openSUSE...):
        sudo zypper in python3-devel  # for python3.x installs

    For apk (Alpine...):

        # This is a departure from the normal Alpine naming
        # scheme, which uses py2- and py3- prefixes

        sudo apk add python3-dev  # for python3.x installs

    For apt-cyg (Cygwin...):
        apt-cyg install python3-devel  # for python3.x installs
        
    */

#ifdef HAVE_PYTHON
    
    if ((ext == PM3_PY) && (searchFile(&script_path, PYTHON_SCRIPTS_SUBDIR, preferredName, ".py", true) == PM3_SUCCESS)) {
        
        PrintAndLogEx(SUCCESS, "executing python " _YELLOW_("%s"), script_path);
        PrintAndLogEx(SUCCESS, "args " _YELLOW_("'%s'"), arguments);

        wchar_t *program = Py_DecodeLocale(preferredName, NULL);
        if (program == NULL) {
            PrintAndLogEx(ERR, "could not decode " _YELLOW_("%s"), preferredName);
            free(script_path);
            return PM3_ESOFT;
        }

        // optional but recommended
        Py_SetProgramName(program);
        Py_Initialize();

        //int argc, char ** argv
        char *argv[128];
        int argc = split(arguments, argv);
   
        wchar_t *py_args[argc];
        py_args[0] = Py_DecodeLocale(preferredName, NULL);
        for (int i = 0; i < argc; i++) {
            py_args[i+1] = Py_DecodeLocale(argv[i], NULL);
        }

        PySys_SetArgv(argc+1, py_args);
        
        // clean up
        for (int i = 0; i < argc; ++i) {
            free(argv[i]);
        }
        
        // setup search paths.
        set_python_paths();
        
        FILE *f = fopen(script_path, "r");
        if (f == NULL) {
            PrintAndLogEx(ERR, "Could open file " _YELLOW_("%s"), script_path);
            free(script_path);
            return PM3_ESOFT;            
        }

        // to we need to manually call all importmodules?
        //PyImport_ImportModule("requests");
        PyRun_SimpleFileExFlags(f, preferredName, 1, NULL);

        Py_Finalize();
        
        PyMem_RawFree(program);
        free(script_path);
        PrintAndLogEx(SUCCESS, "\nfinished " _YELLOW_("%s"), preferredName);
        return PM3_SUCCESS;
    }
#endif

    // file not found, let's search again to display the error messages
    int ret = PM3_EUNDEF;
    if (ext == PM3_LUA)
        ret = searchFile(&script_path, LUA_SCRIPTS_SUBDIR, preferredName, ".lua", false);

    if (ext == PM3_CMD)
        ret = searchFile(&script_path, CMD_SCRIPTS_SUBDIR, preferredName, ".cmd", false);
#ifdef HAVE_PYTHON
    if (ext == PM3_PY)
        ret = searchFile(&script_path, PYTHON_SCRIPTS_SUBDIR, preferredName, ".py", false);
#endif
    free(script_path);
    return ret;
}

static command_t CommandTable[] = {
    {"help",  CmdHelp,          AlwaysAvailable, "This help"},
    {"list",  CmdScriptList,    AlwaysAvailable, "List available scripts"},
    {"run",   CmdScriptRun,     AlwaysAvailable, "<name> -- execute a script"},
    {NULL, NULL, NULL, NULL}
};

/**
 * Shows some basic help
 * @brief CmdHelp
 * @param Cmd
 * @return
 */
static int CmdHelp(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    PrintAndLogEx(NORMAL, "This is a feature to run Lua-scripts. You can place Lua-scripts within the luascripts/-folder. ");
    return PM3_SUCCESS;
}

/**
 * Finds a matching script-file
 * @brief CmdScript
 * @param Cmd
 * @return
 */
int CmdScript(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}

