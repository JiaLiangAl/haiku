/*
 * Copyright 2017, Andrew Lindesay <apl@lindesay.co.nz>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "ServerIconExportUpdateProcess.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <HttpRequest.h>
#include <Json.h>
#include <Url.h>
#include <support/ZlibCompressionAlgorithm.h>

#include "ServerSettings.h"
#include "StorageUtils.h"
#include "TarArchiveService.h"
#include "ToFileUrlProtocolListener.h"


#define MAX_REDIRECTS 3
#define MAX_FAILURES 2

#define HTTP_STATUS_OK 200
#define HTTP_STATUS_FOUND 302
#define HTTP_STATUS_NOT_MODIFIED 304

#define APP_ERR_NOT_MODIFIED (B_APP_ERROR_BASE + 452)

// 30 seconds
#define TIMEOUT_MICROSECONDS 3e+7


/*! This constructor will locate the cached data in a standardized location */

ServerIconExportUpdateProcess::ServerIconExportUpdateProcess(
	const BPath& localStorageDirectoryPath)
{
	fLocalStorageDirectoryPath = localStorageDirectoryPath;
}


status_t
ServerIconExportUpdateProcess::Run()
{
	BPath tarGzFilePath(tmpnam(NULL));
	status_t result = B_OK;

	fprintf(stdout, "will start fetching icons\n");

	result = _Download(tarGzFilePath);

	if (result != APP_ERR_NOT_MODIFIED) {
		if (result != B_OK)
			return result;

		fprintf(stdout, "delete any existing stored data\n");
		StorageUtils::RemoveDirectoryContents(fLocalStorageDirectoryPath);

		BFile *tarGzFile = new BFile(tarGzFilePath.Path(), O_RDONLY);
		BDataIO* tarIn;

		BZlibDecompressionParameters* zlibDecompressionParameters
			= new BZlibDecompressionParameters();

		result = BZlibCompressionAlgorithm()
			.CreateDecompressingInputStream(tarGzFile,
			    zlibDecompressionParameters, tarIn);

		if (result == B_OK) {
			result = TarArchiveService::Unpack(*tarIn,
			    fLocalStorageDirectoryPath);

			if (result == B_OK) {
				if (0 != remove(tarGzFilePath.Path())) {
					fprintf(stdout, "unable to delete the temporary tgz path; "
					    "%s", tarGzFilePath.Path());
				}
			}
		}

		delete tarGzFile;
	}

	fprintf(stdout, "did complete fetching icons\n");

	return result;
}


status_t
ServerIconExportUpdateProcess::_IfModifiedSinceHeaderValue(BString& headerValue,
	BPath& iconMetaDataPath) const
{
	headerValue.SetTo("");
	struct stat s;

	if (-1 == stat(iconMetaDataPath.Path(), &s)) {
		if (ENOENT != errno)
			 return B_ERROR;

		return B_FILE_NOT_FOUND;
	}

	IconMetaData iconMetaData;
	status_t result = _PopulateIconMetaData(iconMetaData, iconMetaDataPath);

	if (result == B_OK) {
		_TimestampToRfc2822String(iconMetaData.GetDataModifiedTimestamp(),
			headerValue);
	}

	return result;
}


status_t
ServerIconExportUpdateProcess::_IfModifiedSinceHeaderValue(BString& headerValue)
    const
{
	BPath iconMetaDataPath(fLocalStorageDirectoryPath);
	iconMetaDataPath.Append("hicn/info.json");
	return _IfModifiedSinceHeaderValue(headerValue, iconMetaDataPath);
}


status_t
ServerIconExportUpdateProcess::_Download(BPath& tarGzFilePath)
{
	BString urlString = ServerSettings::CreateFullUrl("/__pkgicon/all.tar.gz");
	return _Download(tarGzFilePath, BUrl(urlString), 0, 0);
}


status_t
ServerIconExportUpdateProcess::_Download(BPath& tarGzFilePath, const BUrl& url,
	uint32 redirects, uint32 failures)
{
	if (redirects > MAX_REDIRECTS) {
		fprintf(stdout, "exceeded %d redirects --> failure\n", MAX_REDIRECTS);
		return B_IO_ERROR;
	}

	if (failures > MAX_FAILURES) {
		fprintf(stdout, "exceeded %d failures\n", MAX_FAILURES);
		return B_IO_ERROR;
	}

	fprintf(stdout, "will stream '%s' to [%s]\n", url.UrlString().String(),
		tarGzFilePath.Path());

	bool isSecure = url.Protocol() == BString("https");
	ToFileUrlProtocolListener listener(tarGzFilePath, "icon-export",
		ServerSettings::UrlConnectionTraceLoggingEnabled());
	BUrlContext context;

	BHttpHeaders headers;
	ServerSettings::AugmentHeaders(headers);

	BString ifModifiedSinceHeader;
	status_t ifModifiedSinceHeaderStatus = _IfModifiedSinceHeaderValue(
		ifModifiedSinceHeader);

	if (ifModifiedSinceHeaderStatus == B_OK &&
		ifModifiedSinceHeader.Length() > 0) {
		headers.AddHeader("If-Modified-Since", ifModifiedSinceHeader);
	}

	BHttpRequest request(url, isSecure, "HTTP", &listener, &context);
	request.SetMethod(B_HTTP_GET);
	request.SetHeaders(headers);
	request.SetTimeout(TIMEOUT_MICROSECONDS);

	thread_id thread = request.Run();
	wait_for_thread(thread, NULL);

	const BHttpResult& result = dynamic_cast<const BHttpResult&>(
		request.Result());

	int32 statusCode = result.StatusCode();

	switch (statusCode) {
		case HTTP_STATUS_OK:
			fprintf(stdout, "did complete streaming data\n");
			return B_OK;

		case HTTP_STATUS_NOT_MODIFIED:
			fprintf(stdout, "remote data has not changed since [%s]\n",
				ifModifiedSinceHeader.String());
			return APP_ERR_NOT_MODIFIED;

		case HTTP_STATUS_FOUND: // redirect
		{
			const BHttpHeaders responseHeaders = result.Headers();
			const char *locationValue = responseHeaders["Location"];

			if (NULL != locationValue && 0 != strlen(locationValue)) {
				BUrl location(locationValue);
				fprintf(stdout, "will redirect to; %s\n",
					location.UrlString().String());
				return _Download(tarGzFilePath, location, redirects + 1, 0);
			}

			fprintf(stdout, "unable to find 'Location' header for redirect\n");
			return B_IO_ERROR;
		}

		default:
			if (0 == statusCode || 5 == (statusCode / 100)) {
				fprintf(stdout, "error response from server; %zu --> "
					"retry...\n", statusCode);
				return _Download(tarGzFilePath, url, redirects, failures + 1);
			}

			fprintf(stdout, "unexpected response from server; %zu\n",
				statusCode);
			return B_IO_ERROR;
	}
}


status_t
ServerIconExportUpdateProcess::_PopulateIconMetaData(IconMetaData& iconMetaData,
	BMessage& message) const
{
	status_t result = B_OK;
	double value; // numeric resolution issue?

	if (result == B_OK)
		result = message.FindDouble("createTimestamp", &value);

	if (result == B_OK)
		iconMetaData.SetCreateTimestamp((uint64) value);

	if (result == B_OK)
		result = message.FindDouble("dataModifiedTimestamp", &value);

	if (result == B_OK)
		iconMetaData.SetDataModifiedTimestamp((uint64) value);

	return result;
}


status_t
ServerIconExportUpdateProcess::_PopulateIconMetaData(IconMetaData& iconMetaData,
	BString& jsonString) const
{
	BJson parser;
	BMessage infoMetaDataMessage;
	status_t result = parser.Parse(infoMetaDataMessage, jsonString);

	if (result == B_OK)
		return _PopulateIconMetaData(iconMetaData, infoMetaDataMessage);

	return result;
}


status_t
ServerIconExportUpdateProcess::_PopulateIconMetaData(IconMetaData& iconMetaData,
	BPath& path) const
{

	BString infoMetaDataStr;
	status_t result = StorageUtils::AppendToString(path, infoMetaDataStr);

	if (result == B_OK)
		return _PopulateIconMetaData(iconMetaData, infoMetaDataStr);

	return result;
}


/* The output format for this is suitable for use in the "If-Modified-Since"
 * header.  An example of this output would be;
 * 'Fri, 24 Oct 2014 19:32:27 +0000'
 */

void
ServerIconExportUpdateProcess::_TimestampToRfc2822String(
	uint64_t timestampMillis,
	BString& rfc2822String) const
{
	char output[256];
	output[0] = 0;
	time_t t = (timestampMillis / 1000);
	strftime(output, 256, "%a, %d %b %Y %T %z", gmtime(&t));
	rfc2822String.SetTo(output);
}
