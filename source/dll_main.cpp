/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "version.h"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "hook_manager.hpp"
#include "addon_manager.hpp"
#include <Windows.h>
#include <Psapi.h>
#ifndef NDEBUG
#include <DbgHelp.h>

static PVOID s_exception_handler_handle = nullptr;
#endif

// Export special symbol to identify modules as ReShade instances
extern "C" __declspec(dllexport) const char *ReShadeVersion = VERSION_STRING_PRODUCT;

HMODULE g_module_handle = nullptr;
std::filesystem::path g_reshade_dll_path;
std::filesystem::path g_reshade_base_path;
std::filesystem::path g_target_executable_path;

/// <summary>
/// Checks whether the current operation system is Windows 7 or earlier.
/// </summary>
bool is_windows7()
{
	ULONGLONG condition = 0;
	VER_SET_CONDITION(condition, VER_MAJORVERSION, VER_LESS_EQUAL);
	VER_SET_CONDITION(condition, VER_MINORVERSION, VER_LESS_EQUAL);

	OSVERSIONINFOEX verinfo_windows7 = { sizeof(verinfo_windows7), 6, 1 };
	return VerifyVersionInfo(&verinfo_windows7, VER_MAJORVERSION | VER_MINORVERSION, condition) != FALSE;
}

/// <summary>
/// Expands any environment variables in the path (like "%USERPROFILE%") and checks whether it points towards an existing directory.
/// </summary>
static bool resolve_env_path(std::filesystem::path &path, const std::filesystem::path &base = g_reshade_dll_path.parent_path())
{
	WCHAR buf[4096];
	if (!ExpandEnvironmentStringsW(path.c_str(), buf, ARRAYSIZE(buf)))
		return false;

	path = buf;
	path = base / path;
	path = path.lexically_normal();
	if (!path.has_stem()) // Remove trailing slash
		path = path.parent_path();

	std::error_code ec;
	return std::filesystem::is_directory(path, ec);
}

/// <summary>
/// Returns the path that should be used as base for relative paths.
/// </summary>
std::filesystem::path get_base_path(bool default_to_target_executable_path = false)
{
	std::filesystem::path result;

	if (reshade::global_config().get("INSTALL", "BasePath", result) && resolve_env_path(result))
		return result;

	WCHAR buf[4096];
	if (GetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", buf, ARRAYSIZE(buf)) && resolve_env_path(result = buf))
		return result;

	return default_to_target_executable_path ? g_target_executable_path.parent_path() : g_reshade_dll_path.parent_path();
}

/// <summary>
/// Returns the path to the "System32" directory or the module path from global configuration if it exists.
/// </summary>
std::filesystem::path get_system_path()
{
	static std::filesystem::path result;
	if (!result.empty())
		return result; // Return the cached path if it exists

	if (reshade::global_config().get("INSTALL", "ModulePath", result) && resolve_env_path(result))
		return result;

	WCHAR buf[4096];
	if (GetEnvironmentVariableW(L"RESHADE_MODULE_PATH_OVERRIDE", buf, ARRAYSIZE(buf)) && resolve_env_path(result = buf))
		return result;

	// First try environment variable, use system directory if it does not exist or is empty
	GetSystemDirectoryW(buf, ARRAYSIZE(buf));
	return result = buf;
}

/// <summary>
/// Returns the path to the module file identified by the specified <paramref name="module"/> handle.
/// </summary>
std::filesystem::path get_module_path(HMODULE module)
{
	WCHAR buf[4096];
	return GetModuleFileNameW(module, buf, ARRAYSIZE(buf)) ? buf : std::filesystem::path();
}

#ifndef RESHADE_TEST_APPLICATION

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			// Do NOT call 'DisableThreadLibraryCalls', since we are linking against the static CRT, which requires the thread notifications to work properly
			// It does not do anything when static TLS is used anyway, which is the case (see https://docs.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-disablethreadlibrarycalls)
			g_module_handle = hModule;
			g_reshade_dll_path = get_module_path(hModule);
			g_target_executable_path = get_module_path(nullptr);

			const std::filesystem::path module_name = g_reshade_dll_path.stem();

			const bool is_d3d = _wcsnicmp(module_name.c_str(), L"d3d", 3) == 0;
			const bool is_dxgi = _wcsicmp(module_name.c_str(), L"dxgi") == 0;
			const bool is_opengl = _wcsicmp(module_name.c_str(), L"opengl32") == 0;
			const bool is_dinput = _wcsnicmp(module_name.c_str(), L"dinput", 6) == 0;

			const bool default_base_to_target_executable_path = !is_d3d && !is_dxgi && !is_opengl && !is_dinput;

			// When ReShade is not loaded by proxy, only actually load when a configuration file exists for the target executable
			// This e.g. prevents loading the implicit Vulkan layer when not explicitly enabled for an application
			if (default_base_to_target_executable_path)
			{
				std::error_code ec;
				if (!std::filesystem::exists(reshade::global_config().path(), ec))
				{
#ifndef NDEBUG
					// Log was not yet opened at this point, so this only writes to debug output
					LOG(WARN) << "ReShade was not enabled for " << g_target_executable_path << "! Aborting initialization ...";
#endif
					return FALSE; // Make the 'LoadLibrary' call that loaded this instance fail
				}
			}

			g_reshade_base_path = get_base_path(default_base_to_target_executable_path);

			bool enable_log = true;
			if (reshade::global_config().get("INSTALL", "EnableLogging", enable_log); enable_log)
			{
				std::filesystem::path log_path = g_reshade_base_path / L"ReShade.log";
				if (!reshade::log::open_log_file(log_path))
				{
					// Try a different file if the default failed to open (e.g. because currently in use by another ReShade instance)
					std::filesystem::path log_filename = L"ReShade_";
					log_filename += g_target_executable_path.stem();
					log_filename += L".log";

					log_path.replace_filename(log_filename);

					reshade::log::open_log_file(log_path);
				}
			}

			LOG(INFO) << "Initializing crosire's ReShade version '" VERSION_STRING_FILE "' "
#ifndef _WIN64
				"(32-bit) "
#else
				"(64-bit) "
#endif
				"loaded from " << g_reshade_dll_path << " into " << g_target_executable_path << " ...";

			// Check if another ReShade instance was already loaded into the process
			if (HMODULE modules[1024]; K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &fdwReason)) // Use kernel32 variant which is available in DllMain
			{
				for (DWORD i = 0; i < std::min<DWORD>(fdwReason / sizeof(HMODULE), ARRAYSIZE(modules)); ++i)
				{
					if (modules[i] != hModule && GetProcAddress(modules[i], "ReShadeVersion") != nullptr)
					{
						LOG(WARN) << "Another ReShade instance was already loaded from " << get_module_path(modules[i]) << "! Aborting initialization ...";
						return FALSE; // Make the "LoadLibrary" call that loaded this instance fail
					}
				}
			}

#ifndef NDEBUG
			if (reshade::global_config().get("INSTALL", "DumpExceptions"))
			{
				CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&LoadLibraryW), const_cast<LPVOID>(static_cast<LPCVOID>(L"dbghelp.dll")), 0, nullptr);

				s_exception_handler_handle = AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS ex) -> LONG {
					// Ignore debugging and some common language exceptions
					if (const DWORD code = ex->ExceptionRecord->ExceptionCode;
						code == CONTROL_C_EXIT || code == 0x406D1388 /* SetThreadName */ ||
						code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C || code == STATUS_BREAKPOINT ||
						code == 0xE0434352 /* CLR exception */ ||
						code == 0xE06D7363 /* Visual C++ exception */ ||
						((code ^ 0xE24C4A00) <= 0xFF) /* LuaJIT exception */)
						goto continue_search;

					// Create dump with exception information for the first 100 occurrences
					if (static unsigned int dump_index = 0; dump_index < 100)
					{
						const auto dbghelp = GetModuleHandleW(L"dbghelp.dll");
						if (dbghelp == nullptr)
							goto continue_search;

						const auto dbghelp_write_dump = reinterpret_cast<BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION)>(
							GetProcAddress(dbghelp, "MiniDumpWriteDump"));
						if (dbghelp_write_dump == nullptr)
							goto continue_search;

						char dump_name[] = "exception_00.dmp";
						dump_name[10] = '0' + static_cast<char>(dump_index / 10);
						dump_name[11] = '0' + static_cast<char>(dump_index % 10);

						const HANDLE file = CreateFileA(dump_name, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
						if (file == INVALID_HANDLE_VALUE)
							goto continue_search;

						MINIDUMP_EXCEPTION_INFORMATION info;
						info.ThreadId = GetCurrentThreadId();
						info.ExceptionPointers = ex;
						info.ClientPointers = TRUE;

						if (dbghelp_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &info, nullptr, nullptr))
							dump_index++;
						else
							LOG(ERROR) << "Failed to write minidump!";

						CloseHandle(file);
					}

				continue_search:
					return EXCEPTION_CONTINUE_SEARCH;
				});
			}
#endif

			if (reshade::global_config().get("INSTALL", "PreventUnloading"))
			{
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(hModule), &hModule);
			}

			// Register modules to hook
			{
				reshade::hooks::register_module(L"user32.dll");

#if RESHADE_ADDON_LITE
				// Disable network hooks when requested through an environment variable and always disable add-ons in that case
				if (GetEnvironmentVariableW(L"RESHADE_DISABLE_NETWORK_HOOK", nullptr, 0))
				{
					extern volatile long g_network_traffic;
					// Special value to indicate that add-ons should never be enabled
					g_network_traffic = std::numeric_limits<long>::max();
					reshade::addon_enabled = false;
				}
				else
				{
					reshade::hooks::register_module(L"ws2_32.dll");
				}
#endif

				// Only register D3D hooks when module is not called opengl32.dll
				if (!is_opengl)
				{
					reshade::hooks::register_module(get_system_path() / L"d2d1.dll");
					reshade::hooks::register_module(get_system_path() / L"d3d9.dll");
					reshade::hooks::register_module(get_system_path() / L"d3d10.dll");
					reshade::hooks::register_module(get_system_path() / L"d3d10_1.dll");
					reshade::hooks::register_module(get_system_path() / L"d3d11.dll");

					// On Windows 7 the d3d12on7 module is not in the system path, so register to hook any d3d12.dll loaded instead
					if (is_windows7() && _wcsicmp(module_name.c_str(), L"d3d12") != 0)
						reshade::hooks::register_module(L"d3d12.dll");
					else
						reshade::hooks::register_module(get_system_path() / L"d3d12.dll");

					reshade::hooks::register_module(get_system_path() / L"dxgi.dll");
				}

				// Only register OpenGL hooks when module is not called any D3D module name
				if (!is_d3d && !is_dxgi)
				{
					reshade::hooks::register_module(get_system_path() / L"opengl32.dll");
				}

				// Do not register Vulkan hooks, since Vulkan layering mechanism is used instead

#ifndef _WIN64
				reshade::hooks::register_module(L"vrclient.dll");
#else
				reshade::hooks::register_module(L"vrclient_x64.dll");
#endif

				// Always register DirectInput 1-7 module
				reshade::hooks::register_module(get_system_path() / L"dinput.dll");
				// Register DirectInput 8 module in case it was used to load ReShade (but ignore otherwise)
				if (_wcsicmp(module_name.c_str(), L"dinput8") == 0)
					reshade::hooks::register_module(get_system_path() / L"dinput8.dll");
			}

			LOG(INFO) << "Initialized.";
			break;
		}
		case DLL_PROCESS_DETACH:
		{
			LOG(INFO) << "Exiting ...";

#if RESHADE_ADDON
			if (reshade::has_loaded_addons())
				LOG(WARN) << "Add-ons are still loaded! Application may crash on exit.";
#endif

			reshade::hooks::uninstall();

			// Module is now invalid, so break out of any message loops that may still have it in the call stack (see 'HookGetMessage' implementation in input.cpp)
			// This is necessary since a different thread may have called into the 'GetMessage' hook from ReShade, but may not receive a message until after the ReShade module was unloaded
			// At that point it would return to code that was already unloaded and crash
			// Hooks were already uninstalled now, so after returning from any existing 'GetMessage' hook call, application will call the real one next and things continue to work
			g_module_handle = nullptr;
			// This duration has to be slightly larger than the timeout in 'HookGetMessage' to ensure success
			// It should also be large enough to cover any potential other calls to previous hooks that may still be in flight from other threads
			Sleep(1000);

#ifndef NDEBUG
			if (s_exception_handler_handle != nullptr)
				RemoveVectoredExceptionHandler(s_exception_handler_handle);
#endif

			LOG(INFO) << "Finished exiting.";
			break;
		}
	}

	return TRUE;
}

#endif
