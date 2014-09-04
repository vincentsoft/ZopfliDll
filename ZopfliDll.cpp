// ZopfliDll.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "zopfli.h"
#include "httpcompression.h"

static HMODULE gzip;
static IisInitCompression ProcInitCompression = NULL;
static IisCreateCompression ProcCreateCompression = NULL;
static IisCompress ProcCompress = NULL;
static IisDestroyCompression ProcDestroyCompression = NULL;
static IisDeInitCompression ProcDeInitCompression = NULL;

//
// Initialize compression scheme
// When used with IIS, InitCompression is called once as soon
// as compression scheme dll is loaded by IIS compression module
//
extern "C" __declspec(dllexport)
HRESULT
WINAPI
InitCompression(
VOID
)
{
	if (gzip == NULL)
	{
		auto gzip_env_path = L"%Windir%\\system32\\inetsrv\\gzip.dll";
		wchar_t gzip_path[256];
		auto path_len = ExpandEnvironmentStrings(gzip_env_path, gzip_path, 255);
		if (path_len == 0 || path_len > 255) return E_INVALIDARG;
		gzip = LoadLibrary(gzip_path);
		if (gzip == NULL) return E_FAIL;
	}

	ProcInitCompression = (IisInitCompression)GetProcAddress(gzip, "InitCompression");
	if (ProcInitCompression == NULL) return E_FAIL;
	ProcCreateCompression = (IisCreateCompression)GetProcAddress(gzip, "CreateCompression");
	if (ProcCreateCompression == NULL) return E_FAIL;
	ProcCompress = (IisCompress)GetProcAddress(gzip, "Compress");
	if (ProcCompress == NULL) return E_FAIL;
	ProcDestroyCompression = (IisDestroyCompression)GetProcAddress(gzip, "DestroyCompression");
	if (ProcDestroyCompression == NULL) return E_FAIL;
	ProcDeInitCompression = (IisDeInitCompression)GetProcAddress(gzip, "DeInitCompression");
	if (ProcDeInitCompression == NULL) return E_FAIL;

	return ProcInitCompression();
}

//
// Uninitialize compression scheme
// When used with IIS, this method is called before compression
// scheme dll is unloaded by IIS compression module
//
extern "C" __declspec(dllexport)
VOID
WINAPI
DeInitCompression(
VOID
) 
{
	if (ProcDeInitCompression != NULL) ProcDeInitCompression();
}

typedef struct CompressionContext
{
	ZopfliOptions options;
	BYTE *input_buffer;
	size_t input_buffer_size;
	unsigned char *output_buffer;
	size_t output_buffer_size;
	size_t output_used;
	PVOID iis_compression_context;
} CompressionContext;

static const int zopfli_iterations[] = { 1, 5, 10, 15, 20 };

//
// Create a new compression context
//
extern "C" __declspec(dllexport)
HRESULT
WINAPI
CreateCompression(
OUT PVOID *context,
IN  ULONG reserved
)
{
	auto cc = new CompressionContext();
	if (cc == NULL) return E_OUTOFMEMORY;
	ZopfliInitOptions(&cc->options);
	*context = cc;
	
	if (ProcCreateCompression == NULL) return E_FAIL;
	return ProcCreateCompression(&cc->iis_compression_context, reserved);
}

//
// Compress data
//
extern "C" __declspec(dllexport)
HRESULT WINAPI Compress(
	IN  OUT PVOID           context,            // compression context
	IN      CONST BYTE *    input_buffer,       // input buffer
	IN      LONG            input_buffer_size,  // size of input buffer
	IN      PBYTE           output_buffer,      // output buffer
	IN      LONG            output_buffer_size, // size of output buffer
	OUT     PLONG           input_used,         // amount of input buffer used
	OUT     PLONG           output_used,        // amount of output buffer used
	IN      INT             compression_level   // compression level (1...10)
	)
{
	if (compression_level < 0 || compression_level > 10
		|| context == NULL)
		return E_INVALIDARG;

	auto cc = (CompressionContext *)context;

	if (compression_level <= 5)
	{
		return ProcCompress(cc->iis_compression_context,
			input_buffer, input_buffer_size, output_buffer, output_buffer_size, 
			input_used, output_used, compression_level * 2);
	}

	*input_used = 0;
	*output_used = 0;

	if (input_buffer_size > 0)
	{
		cc->input_buffer = (BYTE *)realloc(cc->input_buffer, cc->input_buffer_size + input_buffer_size);
		if (cc->input_buffer == NULL) return E_OUTOFMEMORY;
		auto res = memcpy_s(cc->input_buffer + cc->input_buffer_size, input_buffer_size, input_buffer, input_buffer_size);
		if (res != 0) return E_FAIL;
		cc->input_buffer_size = cc->input_buffer_size + input_buffer_size;

		*input_used = input_buffer_size;
	}
	else
	{
		if (cc->output_used == 0)
		{
			cc->options.numiterations = zopfli_iterations[compression_level - 6];
			ZopfliCompress(&cc->options, ZopfliFormat::ZOPFLI_FORMAT_GZIP, cc->input_buffer, cc->input_buffer_size, &cc->output_buffer, &cc->output_buffer_size);
			if (cc->output_buffer == NULL) return E_OUTOFMEMORY;
			if (cc->output_buffer_size <= 0) return E_FAIL;
		}

		if (cc->output_used == cc->output_buffer_size) return S_FALSE;
	
		auto bytes_left = cc->output_buffer_size - cc->output_used;
		auto bytes_to_copy = output_buffer_size < (LONG)bytes_left ? output_buffer_size : (LONG)bytes_left;
		auto res = memcpy_s(output_buffer, output_buffer_size, cc->output_buffer + cc->output_used, bytes_to_copy);
		if (res != 0) return E_FAIL;
		*output_used = bytes_to_copy;
		cc->output_used += bytes_to_copy;

		if (cc->output_used == cc->output_buffer_size) return S_FALSE;
	}

	return S_OK;
}

//
// Destroy compression context
//
extern "C" __declspec(dllexport)
VOID
WINAPI
DestroyCompression(
IN PVOID context
)
{
	auto cc = (CompressionContext *)context;

	free(cc->input_buffer);
	cc->input_buffer = NULL;
	cc->input_buffer_size = 0;
	free(cc->output_buffer);
	cc->output_buffer = NULL;
	cc->output_buffer_size = 0;
	cc->output_used = 0;

	if (ProcDestroyCompression != NULL) ProcDestroyCompression(cc->iis_compression_context);
	cc->iis_compression_context = NULL;

	delete cc;
}

//
// Reset compression state
// Required export but not used on Windows Vista and Windows Server 2008 - IIS 7.0
// Deprecated and not required export for Windows 7 and Windows Server 2008 R2 - IIS 7.5
//
extern "C" __declspec(dllexport)
HRESULT
WINAPI
ResetCompression(
IN OUT PVOID context
)
{
	return S_OK;
}
