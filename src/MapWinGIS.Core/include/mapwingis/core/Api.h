// SPDX-License-Identifier: MPL-2.0

/**
 * @file Api.h
 * @brief Defines the DLL import/export attribute for the core API.
 *
 * Consumers exchange standard-library types across the DLL boundary and must
 * use the same toolset and CRT as MapWinGIS.Core.dll.
 */
#pragma once

/// Export/import attribute for the public MapWinGIS.Core API.
///
/// The API uses standard library types across the DLL boundary, so consumers
/// must be built with the same toolset and CRT as MapWinGIS.Core.dll. Every
/// binary in the MapWinGIS product satisfies that.
#ifdef MWG_CORE_BUILD
#define MWG_CORE_API __declspec(dllexport)
#else
#define MWG_CORE_API __declspec(dllimport)
#endif
