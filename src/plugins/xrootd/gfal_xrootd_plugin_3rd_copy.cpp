/*
 * Copyright (c) CERN 2013-2015
 *
 * Copyright (c) Members of the EMI Collaboration. 2010-2013
 *  See  http://www.eu-emi.eu/partners for details on the copyright
 *  holders.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <gfal_plugins_api.h>
#include "gfal_xrootd_plugin_interface.h"
#include "gfal_xrootd_plugin_utils.h"

#undef TRUE
#undef FALSE

#include <XrdCl/XrdClCopyProcess.hh>
#include <XrdVersion.hh>


class CopyFeedback: public XrdCl::CopyProgressHandler
{
public:
    CopyFeedback(gfal2_context_t context, gfalt_params_t p, bool isThirdParty) :
            context(context), params(p), startTime(0), isThirdParty(isThirdParty)
    {
        memset(&status, 0x00, sizeof(status));
    }

    virtual ~CopyFeedback()
    {
    }

    void BeginJob(uint16_t jobNum, uint16_t jobTotal, const XrdCl::URL *source,
            const XrdCl::URL *destination)
    {
        this->startTime = time(NULL);
        this->source = source->GetURL();
        this->destination = destination->GetURL();

        plugin_trigger_event(this->params, xrootd_domain, GFAL_EVENT_NONE,
                GFAL_EVENT_TRANSFER_ENTER, "%s => %s", this->source.c_str(),
                this->destination.c_str());

        if (this->isThirdParty) {
            plugin_trigger_event(params, xrootd_domain,
                GFAL_EVENT_NONE, GFAL_EVENT_TRANSFER_TYPE, GFAL_TRANSFER_TYPE_PULL);
        }
        else {
            plugin_trigger_event(params, xrootd_domain,
                GFAL_EVENT_NONE, GFAL_EVENT_TRANSFER_TYPE, GFAL_TRANSFER_TYPE_STREAMED);
        }
    }

#if XrdMajorVNUM(XrdVNUMBER) == 4
    void EndJob(uint16_t jobNum, const XrdCl::PropertyList* result)
    {
        std::ostringstream msg;
        msg << "Job finished";

        if (result->HasProperty("status")) {
            XrdCl::XRootDStatus status;
            result->Get("status", status);
            msg << ", " << status.ToStr();
        }
        if (result->HasProperty("realTarget")) {
            std::string value;
            result->Get("realTarget", value);
            msg << ", Real target: " << value;
        }

        plugin_trigger_event(this->params, xrootd_domain, GFAL_EVENT_NONE,
                GFAL_EVENT_TRANSFER_EXIT, "%s", msg.str().c_str());
    }
#else
    void EndJob(const XrdCl::XRootDStatus &status)
    {
        plugin_trigger_event(this->params, xrootd_domain,
                GFAL_EVENT_NONE, GFAL_EVENT_TRANSFER_EXIT,
                "%s", status.ToStr().c_str());
    }
#endif

#if XrdMajorVNUM(XrdVNUMBER) == 4
    void JobProgress(uint16_t jobNum, uint64_t bytesProcessed,
            uint64_t bytesTotal)
#else
    void JobProgress(uint64_t bytesProcessed, uint64_t bytesTotal)
#endif
    {
        time_t now = time(NULL);
        time_t elapsed = now - this->startTime;

        this->status.status = 0;
        this->status.bytes_transfered = bytesProcessed;
        this->status.transfer_time = elapsed;
        if (elapsed > 0)
            this->status.average_baudrate = bytesProcessed / elapsed;
        this->status.instant_baudrate = this->status.average_baudrate;

        plugin_trigger_monitor(this->params, &this->status, this->source.c_str(), this->destination.c_str());
    }

    bool ShouldCancel()
    {
        return gfal2_is_canceled(this->context);
    }

private:
    gfal2_context_t context;
    gfalt_params_t params;

    _gfalt_transfer_status status;
    time_t startTime;

    std::string source, destination;
    bool isThirdParty;
};



int gfal_xrootd_3rdcopy_check(plugin_handle plugin_data,
        gfal2_context_t context, const char* src, const char* dst,
        gfal_url2_check check)
{
    if (check != GFAL_FILE_COPY && check != GFAL_BULK_COPY)
        return 0;

    bool src_is_root = strncmp(src, "root://", 7) == 0 ||
                       strncmp(src, "xroot://", 8) == 0;
    bool dst_is_root = strncmp(dst, "root://", 7) == 0 ||
                       strncmp(dst, "xroot://", 8) == 0;
    bool src_is_file = strncmp(src, "file://", 7) == 0;
    bool dst_is_file = strncmp(dst, "file://", 7) == 0;

    if (src_is_root) {
        return dst_is_root || dst_is_file;
    }
    else if (dst_is_root) {
        return src_is_root || src_is_file;
    }
    return false;
}


static void gfal_xrootd_3rd_init_url(gfal2_context_t context, XrdCl::URL& xurl,
        const char* url, const char* token)
{
    xurl.FromString(prepare_url(context, url));
    if (token) {
        XrdCl::URL::ParamsMap params;
        params.insert(std::make_pair("svcClass", token));
        xurl.SetParams(params);
    }
}

/// Clean dst, update err if failed during cleanup with something else than ENOENT,
/// returns always -1 for convenience
static int gfal_xrootd_copy_cleanup(plugin_handle plugin_data, const char* dst, GError** err)
{
    GError *unlink_err = NULL;
    if ((*err)->code != EEXIST) {
        if (gfal_xrootd_unlinkG(plugin_data, dst, &unlink_err) != 0) {
            if (unlink_err->code != ENOENT) {
                gfal2_log(G_LOG_LEVEL_WARNING,
                         "When trying to clean the destination: %s", unlink_err->message);
            }
            g_error_free(unlink_err);
        }
        else {
            gfal2_log(G_LOG_LEVEL_INFO, "Destination file removed");
        }
    }
    else {
        gfal2_log(G_LOG_LEVEL_DEBUG, "The transfer failed because the file exists. Do not clean!");
    }
    return -1;
}

int gfal_xrootd_3rd_copy_bulk(plugin_handle plugin_data,
        gfal2_context_t context, gfalt_params_t params, size_t nbfiles,
        const char* const * srcs, const char* const * dsts,
        const char* const * checksums, GError** op_error,
        GError*** file_errors)
{
    GError* internalError = NULL;
    char _checksumType[64] = { 0 };
    char _checksumValue[512] = { 0 };
    bool isThirdParty = false;

    gfalt_checksum_mode_t checksumMode = gfalt_get_checksum(params,
        _checksumType, sizeof(_checksumType),
        _checksumValue, sizeof(_checksumValue), NULL);

    //set GFALT_CHECKSUM_BOTH by default if it's not set by the user but the user passes either the type or the value
    if ((!checksumMode) && (_checksumType[0] || _checksumValue[0])) {
        checksumMode = GFALT_CHECKSUM_BOTH;
    }
    XrdCl::CopyProcess copy_process;
#if XrdMajorVNUM(XrdVNUMBER) == 4 ||  XrdMajorVNUM(XrdVNUMBER) == 100
    std::vector<XrdCl::PropertyList> results;
    for (size_t i = 0; i < nbfiles; ++i) {
        results.push_back(XrdCl::PropertyList());
    }
#endif

    const char* src_spacetoken =  gfalt_get_src_spacetoken(params, NULL);
    const char* dst_spacetoken =  gfalt_get_dst_spacetoken(params, NULL);

    for (size_t i = 0; i < nbfiles; ++i) {
        XrdCl::URL source_url, dest_url;
        gfal_xrootd_3rd_init_url(context, source_url, srcs[i], src_spacetoken);
        gfal_xrootd_3rd_init_url(context, dest_url, dsts[i], dst_spacetoken);

#if XrdMajorVNUM(XrdVNUMBER) == 4 || XrdMajorVNUM(XrdVNUMBER) == 100
        XrdCl::PropertyList job;

        job.Set("source", source_url.GetURL());
        job.Set("target", dest_url.GetURL());
        job.Set("force", gfalt_get_replace_existing_file(params, NULL));
        job.Set("posc", true);
        if ((source_url.GetProtocol() == "root") && (dest_url.GetProtocol() == "root")) {
            job.Set("thirdParty", "only");
            isThirdParty = true;
        }
        else {
            job.Set("thirdParty", "first");
        }
        job.Set("tpcTimeout", gfalt_get_timeout(params, NULL));
#else
        XrdCl::JobDescriptor job;

        job.source = source_url;
        job.target = dest_url;
        job.force = gfalt_get_replace_existing_file(params, NULL);;
        job.posc = true;

        if ((source_url.GetProtocol() == "root") && (dest_url.GetProtocol() == "root")) {
            job.thirdParty = true;
            job.thirdPartyFallBack = false;
            isThirdParty = true;
        }
        else {
            job.thirdParty = false;
            job.thirdPartyFallBack = false;
        }

        job.checkSumPrint = false;
#endif

        if (checksumMode) {
            char checksumType[64] = { 0 };
            char checksumValue[512] = { 0 };
            char **chks = g_strsplit(checksums[i], ":", 2);
            char *s = chks[1];
            while (*s && *s == '0') s++;
            strncpy(checksumType, chks[0], sizeof(chks[0]));
            strncpy(checksumValue, s, sizeof(s));
            checksumType[63] = checksumValue[511] = '\0';
            g_strfreev(chks);
	    gfal2_log(G_LOG_LEVEL_DEBUG, "Predefined Checksum Type: %s", checksumType);
            gfal2_log(G_LOG_LEVEL_DEBUG, "Predefined Checksum Value: %s", checksumValue);
            if (!checksumType[0]) {
                char* defaultChecksumType = gfal2_get_opt_string(context, XROOTD_CONFIG_GROUP, XROOTD_DEFAULT_CHECKSUM, &internalError);
                if (internalError) {
                    gfal2_set_error(op_error, xrootd_domain, internalError->code, __func__,
                            "%s", internalError->message);
                    g_error_free(internalError);
                    return -1;
                }

                g_strlcpy(checksumType, defaultChecksumType, sizeof(checksumType));
                g_free(defaultChecksumType);
            }

#if XrdMajorVNUM(XrdVNUMBER) == 4 ||  XrdMajorVNUM(XrdVNUMBER) == 100
            switch (checksumMode) {
                case GFALT_CHECKSUM_BOTH:
                    job.Set("checkSumMode", "end2end");
                    break;
                case GFALT_CHECKSUM_TARGET:
                    job.Set("checkSumMode", "target");
                    break;
                case GFALT_CHECKSUM_SOURCE:
                    job.Set("checkSumMode", "source");
                    break;
                default:
                    job.Set("checkSumMode", "none");
            }
            job.Set("checkSumType", predefined_checksum_type_to_lower(checksumType));
            job.Set("checkSumPreset", checksumValue);
#else
            job.checkSumType = predefined_checksum_type_to_lower(checksumType);
            job.checkSumPreset = checksumValue;
#endif
        }

#if XrdMajorVNUM(XrdVNUMBER) == 4 || XrdMajorVNUM(XrdVNUMBER) == 100
        copy_process.AddJob(job, &(results[i]));
#else
        copy_process.AddJob(&job);
#endif

    }

    // Configuration job
#if XrdMajorVNUM(XrdVNUMBER) == 4 ||  XrdMajorVNUM(XrdVNUMBER) == 100
    int parallel = gfal2_get_opt_integer_with_default(context,
            XROOTD_CONFIG_GROUP, XROOTD_PARALLEL_COPIES,
            20);

    XrdCl::PropertyList config_job;
    config_job.Set("jobType", "configuration");
    config_job.Set("parallel", parallel);
    copy_process.AddJob(config_job, NULL);
#endif

    XrdCl::XRootDStatus status = copy_process.Prepare();
    if (!status.IsOK()) {
        gfal2_set_error(op_error, xrootd_domain,
                xrootd_errno_to_posix_errno(status.errNo), __func__,
                "Error on XrdCl::CopyProcess::Prepare(): %s", status.ToStr().c_str());
        return -1;
    }

    CopyFeedback feedback(context, params, isThirdParty);
    status = copy_process.Run(&feedback);

    // On bulk operations, even if there is one single failure we will get it
    // here, so ignore!
    if (nbfiles == 1 && !status.IsOK()) {
        gfal2_set_error(op_error, xrootd_domain,
                xrootd_errno_to_posix_errno(status.errNo), __func__,
                "Error on XrdCl::CopyProcess::Run(): %s", status.ToStr().c_str());
        return gfal_xrootd_copy_cleanup(plugin_data, dsts[0],op_error);
    }

    // For bulk operations, here we do get the actual status per file
    int n_failed = 0;
#if XrdMajorVNUM(XrdVNUMBER) == 4
    *file_errors = g_new0(GError*, nbfiles);
    for (size_t i = 0; i < nbfiles; ++i) {
        status = results[i].Get<XrdCl::XRootDStatus>("status");
        if (!status.IsOK()) {
            gfal2_set_error(&((*file_errors)[i]), xrootd_domain,
                    xrootd_errno_to_posix_errno(status.errNo), __func__,
                    "Error on XrdCl::CopyProcess::Run(): %s", status.ToStr().c_str());
            gfal_xrootd_copy_cleanup(plugin_data, dsts[i],file_errors[i]);
            ++n_failed;
        }
    }
#endif
    return -n_failed;
}


int gfal_xrootd_3rd_copy(plugin_handle plugin_data, gfal2_context_t context,
        gfalt_params_t params, const char* src, const char* dst, GError** err)
{
    GError* op_error = NULL;
    GError** file_error = NULL;

    char checksumType[64] = { 0 };
    char checksumValue[512] = { 0 };
    gfalt_get_checksum(params, checksumType, sizeof(checksumType),
            checksumValue, sizeof(checksumValue),
            NULL);

    char *checksumConcat[1];
    checksumConcat[0] = g_strdup_printf("%s:%s", checksumType, checksumValue);

    int ret = gfal_xrootd_3rd_copy_bulk(plugin_data,
            context, params, 1,
            &src, &dst, checksumConcat,
            &op_error, &file_error);

    g_free(checksumConcat[0]);

    if (ret < 0) {
        if (op_error) {
            gfal2_propagate_prefixed_error(err, op_error, __func__);
        }
        else if (file_error) {
            gfal2_propagate_prefixed_error(err, file_error[0], __func__);
            g_free(file_error);
        }
    }

    return ret;
}
