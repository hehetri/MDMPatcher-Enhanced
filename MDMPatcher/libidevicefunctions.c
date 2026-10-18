

#include "libidevicefunctions.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include <libirecovery.h>
#include <readline/readline.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/property_list_service.h>
#include <readline/history.h>
#include <math.h>
#include <sys/stat.h>
#include "utils.h"
#include <plist/plist.h>
#include <util.h>
#include <libimobiledevice/mobileactivation.h>

static const char *domains[] = {
    "com.apple.disk_usage",
    "com.apple.disk_usage.factory", /// Disk usage
    "com.apple.mobile.battery", // Battery % rn
/* FIXME: For some reason lockdownd segfaults on this, works sometimes though . */
    "com.apple.mobile.debug",
    "com.apple.SystemConfiguration",
    "com.apple.iqagent",
    "com.apple.purplebuddy", /// useless info
    "com.apple.PurpleBuddy", /// internal settings detail
    "com.apple.mobile.chaperone",
    "com.apple.mobile.third_party_termination",
    "com.apple.mobile.lockdownd",
    "com.apple.mobile.lockdown_cache",
    "com.apple.xcode.developerdomain",
    "com.apple.international", /// language
    "com.apple.mobile.data_sync",
    "com.apple.mobile.tethered_sync",
    "com.apple.mobile.mobile_application_usage",
    "com.apple.mobile.backup",
    "com.apple.mobile.nikita",
    "com.apple.mobile.restriction",
    "com.apple.mobile.user_preferences",
    "com.apple.mobile.sync_data_class",
    "com.apple.mobile.software_behavior", // SwBh ChinaBlock...
    "com.apple.mobile.iTunes.SQLMusicLibraryPostProcessCommands",
    "com.apple.mobile.iTunes.accessories", /// nothing
    "com.apple.mobile.internal", /**< iOS 4.0+ */ /// factory check build/os
    "com.apple.mobile.wireless_lockdown", /**< iOS 4.0+ */ // remote sysnc url
    "com.apple.fairplay", /// Nothing
    "com.apple.iTunes",
    "com.apple.mobile.iTunes.store",
    "com.apple.mobile.iTunes",
    NULL
};

static int is_domain_known(const char *domain)
{
    int i = 0;
    while (domains[i] != NULL) {
        if (strstr(domain, domains[i++])) {
            return 1;
        }
    }
    return 0;
}

static irecv_client_t dev = NULL;
irecv_client_t client = NULL;
uint64_t ecid = 0;
irecv_error_t errors = 0;
char *output_xml = NULL;
int progress_cb(irecv_client_t client, const irecv_event_t* event);
void send_progress(double progress);
void send_progress_fz(unsigned int progress);
irecv_device_t device = NULL;


static swift_callbacks callback;
static swift_progress prog;

static const char* mode_to_str(int mode) {
    switch (mode) {
        case IRECV_K_RECOVERY_MODE_1:
        case IRECV_K_RECOVERY_MODE_2:
        case IRECV_K_RECOVERY_MODE_3:
        case IRECV_K_RECOVERY_MODE_4:
            return "Recovery";
            break;
        case IRECV_K_DFU_MODE:
            return "DFU";
            break;
        case IRECV_K_WTF_MODE:
            return "WTF";
            break;
        default:
            return "Unknown";
            break;
    }
}

extern void callback_setup(const swift_callbacks * callbacks) {
    callback = *callbacks;
}

extern void progress_setup(const swift_progress * callbacks) {
    prog = *callbacks;
}

int progress_cb(irecv_client_t client, const irecv_event_t* event) {
    if (event->type == IRECV_PROGRESS) {
        send_progress(event->progress);
    }
    
    return 0;
}




#define FORMAT_KEY_VALUE 1
#define FORMAT_XML 2
#define TOOL_NAME "ideviceinfo"


/// Customized Section

char *getdeviceInformation(void)
{
    lockdownd_client_t lockdown = NULL;
    idevice_t device = NULL;
    plist_t node = NULL;
    char *xml_doc = NULL;
    uint32_t xml_length = 0;
    idevice_error_t device_result;
    lockdownd_error_t lockdown_result;

    device_result = idevice_new_with_options(
        &device, NULL, IDEVICE_LOOKUP_USBMUX);
    if (device_result != IDEVICE_E_SUCCESS || device == NULL) {
        fprintf(stderr, "ERROR: No USB device found (%d).\\n", device_result);
        return strdup("-1");
    }

    lockdown_result = lockdownd_client_new_with_handshake(
        device, &lockdown, "MDMPatcher");
    if (lockdown_result != LOCKDOWN_E_SUCCESS || lockdown == NULL) {
        fprintf(stderr, "ERROR: Could not connect to lockdownd: %s (%d)\\n",
                lockdownd_strerror(lockdown_result), lockdown_result);
        if (lockdown != NULL) {
            lockdownd_client_free(lockdown);
        }
        idevice_free(device);
        return strdup("-1");
    }

    lockdown_result = lockdownd_get_value(lockdown, NULL, NULL, &node);
    if (lockdown_result != LOCKDOWN_E_SUCCESS || node == NULL) {
        fprintf(stderr, "ERROR: Could not read device values from lockdownd (%d).\\n",
                lockdown_result);
        if (node != NULL) {
            plist_free(node);
        }
        lockdownd_client_free(lockdown);
        idevice_free(device);
        return strdup("-1");
    }

    plist_to_xml(node, &xml_doc, &xml_length);

    plist_free(node);
    lockdownd_client_free(lockdown);
    idevice_free(device);

    if (xml_doc == NULL || xml_length == 0) {
        free(xml_doc);
        fprintf(stderr, "ERROR: Could not serialize device information.\\n");
        return strdup("-1");
    }

    /*
     * Ownership contract: the returned string is heap-allocated and belongs
     * to the caller. The Swift caller must copy it to String and then free it.
     */
    return xml_doc;
}

char *getspecialdeviceInformation(const char *domain_)
{
    lockdownd_client_t lockdown = NULL;
    idevice_t device = NULL;
    plist_t node = NULL;
    char *xml_doc = NULL;
    uint32_t xml_length = 0;
    idevice_error_t device_result;
    lockdownd_error_t lockdown_result;

    if (domain_ == NULL || domain_[0] == '\0') {
        fprintf(stderr, "ERROR: A Lockdown domain is required.\n");
        return strdup("-1");
    }

    device_result = idevice_new_with_options(
        &device, NULL, IDEVICE_LOOKUP_USBMUX);
    if (device_result != IDEVICE_E_SUCCESS || device == NULL) {
        fprintf(stderr, "ERROR: No USB device found (%d).\n", device_result);
        return strdup("-1");
    }

    lockdown_result = lockdownd_client_new_with_handshake(
        device, &lockdown, "MDMPatcher");
    if (lockdown_result != LOCKDOWN_E_SUCCESS || lockdown == NULL) {
        fprintf(stderr, "ERROR: Could not connect to lockdownd: %s (%d)\n",
                lockdownd_strerror(lockdown_result), lockdown_result);
        if (lockdown != NULL) {
            lockdownd_client_free(lockdown);
        }
        idevice_free(device);
        return strdup("-1");
    }

    if (!is_domain_known(domain_)) {
        fprintf(stderr, "WARNING: Querying a domain not listed in the local allowlist: %s\n",
                domain_);
    }

    lockdown_result = lockdownd_get_value(lockdown, domain_, NULL, &node);
    if (lockdown_result != LOCKDOWN_E_SUCCESS || node == NULL) {
        fprintf(stderr, "ERROR: Could not read Lockdown domain '%s' (%d).\n",
                domain_, lockdown_result);
        if (node != NULL) {
            plist_free(node);
        }
        lockdownd_client_free(lockdown);
        idevice_free(device);
        return strdup("-1");
    }

    plist_to_xml(node, &xml_doc, &xml_length);
    plist_free(node);
    lockdownd_client_free(lockdown);
    idevice_free(device);

    if (xml_doc == NULL || xml_length == 0) {
        free(xml_doc);
        fprintf(stderr, "ERROR: Could not serialize Lockdown domain '%s'.\n", domain_);
        return strdup("-1");
    }

    /* Returned data is heap-allocated; caller must free it. */
    return xml_doc;
}

void send_progress(double progress) {
    if(progress < 0) {
        return;
    }
    if(progress > 100) {
        progress = 100;
    }
    prog.send_output_progress_to_swift(progress);
}


void send_progress_fz(unsigned int progress) {
    if(progress < 0) {
        return;
    }
    if(progress > 100) {
        progress = 100;
    }
    prog.send_output_progress_to_swift((double)progress);
}


void send_text(char *text) {
    int size = snprintf(NULL, 0, "%s", text);
    char * a = malloc(size + 1);
    sprintf(a, "%s", text);
    callback.send_output_to_swift(a);
}
