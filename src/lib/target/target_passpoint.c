/*
Copyright (c) 2022, Askey-IND. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
   3. Neither the name of the Plume Design Inc. nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Plume Design Inc. BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE /* for alloca */

/* std libc */
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* internal */
#include <log.h>
#include <target.h>

/* OpenSync */
#include "util.h"
#include "const.h"
#include "log.h"
#include "kconfig.h"
#include "target.h"

#define GENERATE_FILE_PATH(...) strfmta(__VA_ARGS__)
#define HOSTAPD_CONF_FILE_PATH_NAME(vif) GENERATE_FILE_PATH("/var/run/hostapd-%s.config", vif)
#define READ_FILE_DATA(...) file_geta(__VA_ARGS__)
#define GET_PARAM_VAL(file_data, param_name) ini_geta(file_data, param_name)

#define PASSPOINT_CONF_FILE_PATH_NAME(vif) GENERATE_FILE_PATH("%s/%s", CONFIG_PWM_PASSPOINT_CONFIG_DIR, vif)

#define HOTSPOT_ENABLE "hs20"
#define HOTSPOT_WAN_METRICS "hs20_wan_metrics"
#define HOTSPOT_LIST_3GPP "anqp_3gpp_cell_net"
#define HOTSPOT_NAIREALM "nai_realm"
// statically configured mandatory hotspot values
#define HOTSPOT_INTERWORKING "interworking"
#define HOTSPOT_INTERNET "internet"
#define HOTSPOT_DISABLE_DGAF "disable_dgaf"
#define HOTSPOT_ACCESS_NETWORK_TYPE "access_network_type"

#define PASSPOINT_VALUE_SEPERATOR ";"
#define PASSPOINT_PARAM_MAX_COUNT 1
// Value set as configured in opensync.ovsschema file
#define PASSPOINT_PARAM_ROAMING_CONSORTIUM_MAX_COUNT 8

#define PWM_STRING_SIZE 128
#define PWM_LONG_STRING_SIZE 512
#define PWM_LONG_LONG_STRING_SIZE 1024

#define MODULE_ID LOG_MODULE_ID_TARGET

// in milliseconds
#define WAIT_TIMEOUT 5000

// to initialise prerequisites for passpoint target implementation
static void
target_passpoint_init()
{
    struct stat st = {0};

    // Create CONFIG_PWM_PASSPOINT_CONFIG_DIR if it does not exist
    if (stat(CONFIG_PWM_PASSPOINT_CONFIG_DIR, &st) == -1) {
        mkdir(CONFIG_PWM_PASSPOINT_CONFIG_DIR, 0777);
        LOGI("%s: Created Directory %s for Passpoint Config", __func__, CONFIG_PWM_PASSPOINT_CONFIG_DIR);
    }
}

static char*
get_value_from_config_file(const char *if_name, const char*passpoint_param_name,
                                      char *filename, int max_params_count ) {

    char *value = NULL;
    char *local_value = NULL;

    if(filename) {
        LOGI("%s: File %s opened for reading parameter %s ", __func__, filename, passpoint_param_name);
        const char* data = READ_FILE_DATA(filename) ?: "";
        if (data) {
            // If this value is more than 1, it means, this paramter can have multiple values
            // check for all and return all values with PASSPOINT_VALUE_SEPERATOR
            if ( max_params_count > PASSPOINT_PARAM_MAX_COUNT) {
                int iter = PASSPOINT_PARAM_MAX_COUNT;
                char *parsed_data = strdup(data);
                char *parsed_value = NULL;
                parsed_value = calloc(PWM_LONG_LONG_STRING_SIZE, sizeof(char));
                if( !parsed_value) {
                    LOGE("%s: Memory Allocation Failed ", __func__);
                    return NULL;
                }

                while(iter <= max_params_count) {
                    parsed_data = strstr(parsed_data, passpoint_param_name);
                    if(parsed_data == NULL) {
                        // Parameter not found so return NULL
                        break;
                    }
                    if ( (local_value = GET_PARAM_VAL(parsed_data, passpoint_param_name)) ) {
                        if( iter == 1 )
                            strcat(parsed_value, local_value);
                        else {
                            strcat(parsed_value, ";");
                            strcat(parsed_value, local_value);
                        }

                        LOGI("%s:  For param = %s , value = %s | iter = %d ",
                                __func__, passpoint_param_name, parsed_value, iter);
                    }
                    iter++;
                    parsed_data = parsed_data + strlen(passpoint_param_name);
                }
                if(parsed_value[0] != '\0') {
                    value = strdupa(parsed_value);
                }
                if(parsed_value) {
                    free(parsed_value);
                    parsed_value = NULL;
                }
            } else {
                if ((local_value = GET_PARAM_VAL(data, passpoint_param_name))) {
                    LOGI("%s: Value of %s=%s", __func__, passpoint_param_name, local_value);
                    value = strdupa(local_value);
                }else {
                    LOGW("%s: Value of %s not found", __func__, passpoint_param_name);
                }
            }

        } else
            LOGW("%s: File %s not found ", __func__, filename);
    }

    return value;
}

static bool
get_value_ovsdb_table(const char *table_name, const char* matching_param, const char* matching_param_val,
                             const char* param, char* value) {

    bool ret = false;
    FILE *fp;
    char cmd[PWM_STRING_SIZE] = {0};
    char data[PWM_STRING_SIZE] = {0};

    sprintf(cmd,"ovsh s %s -w %s==%s -c %s | awk '{split($0,a,\" \"); print a[3]}'",table_name, matching_param,
        matching_param_val, param);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(data, sizeof(data), fp) != NULL ) {
            if(strcmp(data, "\n") != 0) {
                strncpy(value, data, strlen(data));
                LOGI("%s: OVSDB table %s get for %s==%s %s=%s ", __func__, table_name,
                        matching_param, matching_param_val, param, value);
                ret =true;
            }
        }
    }

    pclose(fp);
    return ret;
}

static bool
set_value_ovsdb_table(const char *table_name, const char* matching_param, const char* matching_param_val,
                             const char* param, char* value) {

    bool ret = false;
    FILE *fp;
    char cmd[PWM_STRING_SIZE] = {0};
    char data[PWM_STRING_SIZE] = {0};

    sprintf(cmd,"ovsh u %s -w %s==%s  %s:='%s'",table_name, matching_param,
        matching_param_val, param, value);
    fp = popen(cmd, "r");
    if (fp) {
        if (fgets(data, sizeof(data), fp) != NULL ) {
            if(strncmp(data, "1", strlen("1")) ==0) {
                ret =true;
                LOGI("%s: OVSDB table %s updated for %s==%s %s=%s ", __func__, table_name,
                        matching_param, matching_param_val, param, value);
            }
        }
    }

    pclose(fp);
    return ret;
}

void set_hotspot_enable(FILE *fptr, bool state)
{
    if (state)
        fprintf(fptr,"%s=%d\n",HOTSPOT_ENABLE, 1);
    else
        fprintf(fptr,"%s=%d\n",HOTSPOT_ENABLE, 0);

    LOGI("%s: Set %s=%d", __func__, HOTSPOT_ENABLE, state);
}

void set_hotspot_wan_metrics(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->adv_wan_status_exists && conf->adv_wan_symmetric_exists && conf->adv_wan_at_capacity_exists) {
        // Parse and set adv_wan_status(B0-B1)adv_wan_symmetric(B2)adv_wan_at_capacity(B3) in hs20_wan_metrics
        int8_t wan_at = 0;
        int8_t wan_sy = 0;
        int8_t wan_st = 0;

        wan_at = conf->adv_wan_at_capacity << 3;
        wan_sy = conf->adv_wan_symmetric << 2;
        wan_st = conf->adv_wan_status;
        fprintf(fptr, "%s=%02x:0:0:0:0:0\n",HOTSPOT_WAN_METRICS, wan_at | wan_sy | wan_st );
        LOGI("%s: Set %s=%02x:0:0:0:0:0", __func__, HOTSPOT_WAN_METRICS, wan_at | wan_sy | wan_st );
    }
}

void set_hotspot_hessid(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->hessid_exists) {
        fprintf(fptr, "%s=%s\n", CONFIG_PWM_PASSPOINT_HESSID, conf->hessid);
        LOGI("%s: Set %s=%s", __func__, CONFIG_PWM_PASSPOINT_HESSID, conf->hessid);
    }
}

void set_hotspot_domain_name(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->domain_name_exists) {
        fprintf(fptr, "%s=%s\n", CONFIG_PWM_PASSPOINT_DOMAIN_NAME, conf->domain_name);
        LOGI("%s: Set %s=%s", __func__, CONFIG_PWM_PASSPOINT_DOMAIN_NAME, conf->domain_name);
    }
}

void set_hotspot_list_3gpp(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->list_3gpp_len > 0) {
        // Parse and set list_3gpp (V1:V2,V3:V4) as (V1,V2;V3,V4)
        int ppt_i = 0;
        char anqp_val[PWM_STRING_SIZE] = {0};
        char anqp_final_val[PWM_LONG_STRING_SIZE] = {0};
        char tmp_val[PWM_STRING_SIZE] = {0};
        char *tmp_p = NULL;
        char *tmp_savep = NULL;

        while ( conf->list_3gpp[ppt_i][0] != '\0') {
            strncpy(tmp_val, conf->list_3gpp[ppt_i], sizeof(conf->list_3gpp[ppt_i]));
            for (tmp_p = strtok_r(tmp_val, ":", &tmp_savep); tmp_p != NULL; tmp_p = strtok_r(NULL, ":", &tmp_savep)) {
                if( anqp_val[0] != '\0')
                    strcat(anqp_val, ",");

                strcat(anqp_val, tmp_p);
            }
            if( anqp_final_val[0] != '\0')
                strcat(anqp_val, ";");

            strcat(anqp_final_val, anqp_val);
            ppt_i++;
            memset(tmp_val, 0, sizeof(tmp_val));
            memset(anqp_val, 0, sizeof(anqp_val));
        }
        if (anqp_final_val[0] != '\0') {
            fprintf(fptr, "%s=%s\n",HOTSPOT_LIST_3GPP, anqp_final_val);
            LOGI("%s: Set %s=%s", __func__, HOTSPOT_LIST_3GPP, anqp_final_val);
        }
    }
}

void set_hotspot_nairealm(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->nairealm_list_len > 0) {
        // Parse and set nairealm_list (V1,V2,V3) as (0,V1;V2;V3)
        int ppt_i = 0;
        char nairealm_val[PWM_LONG_STRING_SIZE] = {0};

        while ( conf->nairealm_list[ppt_i][0] != '\0') {
            if( nairealm_val[0] != '\0')
                strcat(nairealm_val, ";");

            strcat(nairealm_val, conf->nairealm_list[ppt_i]);
            ppt_i++;
        }
        // Here 0 in nai_realm=0 Means Realm formatted in accordance with IETF RFC 4282
        if (nairealm_val[0] != '\0') {
            fprintf(fptr, "%s=0,%s\n", HOTSPOT_NAIREALM, nairealm_val);
            LOGI("%s: Set %s=0,%s", __func__, HOTSPOT_NAIREALM, nairealm_val);
        }
    }
}

void set_hotspot_roaming_consortium(FILE *fptr, const struct schema_Passpoint_Config *conf)
{
    if (conf->roaming_consortium_len > 0) {
        int ppt_i = 0;
        while ( (PASSPOINT_PARAM_ROAMING_CONSORTIUM_MAX_COUNT > ppt_i ) &&
                conf->roaming_consortium[ppt_i][0] != '\0') {
            fprintf(fptr, "%s=%s\n", CONFIG_PWM_PASSPOINT_ROAMING_CONSORTIUM, conf->roaming_consortium[ppt_i]);
            LOGI("%s: Set %s=%s", __func__, CONFIG_PWM_PASSPOINT_ROAMING_CONSORTIUM, conf->roaming_consortium[ppt_i]);
            ppt_i++;
        }
    }
}

void set_hotspot_other(FILE *fptr, const struct schema_Passpoint_Config *conf) {
    // So we don't know what to set here and in what format
    //if (conf->other_config_len > 0) {
        // Nothing TODO yet
    //}
}

// Set Static Values not defined in schema, but mandatory for Hostapd to configure passpoint.
void set_hotspot_static(FILE *fptr)
{
    // Enable Interworking service
    fprintf(fptr, "%s=%d\n",HOTSPOT_INTERWORKING, 1);
    LOGI("%s: Set %s=%d", __func__,HOTSPOT_INTERWORKING, 1);
    /*
     * Whether the network provides connectivity to the Internet
     * 0 = Unspecified
     * 1 = Network provides connectivity to the Internet
     */
    fprintf(fptr, "%s=%d\n",HOTSPOT_INTERNET, 1);
    LOGI("%s: Set %s=%d", __func__,HOTSPOT_INTERNET, 1);
    /*
     * Disable Downstream Group-Addressed Forwarding (DGAF)
     * This can be used to configure a network where no group-addressed frames are
     * allowed. The AP will not forward any group-address frames to the stations and
     * random GTKs are issued for each station to prevent associated stations from
     * forging such frames to other stations in the BSS.
     */
    fprintf(fptr, "%s=%d\n",HOTSPOT_DISABLE_DGAF , 1);
    LOGI("%s: Set %s=%d", __func__,HOTSPOT_DISABLE_DGAF , 1);
    /*
     * Access Network Type
     * 0 = Private network
     * 1 = Private network with guest access
     * 2 = Chargeable public network
     * 3 = Free public network
     * 4 = Personal device network
     * 5 = Emergency services only network
     * 14 = Test or experimental
     * 15 = Wildcard
     */
    fprintf(fptr, "%s=%d\n",HOTSPOT_ACCESS_NETWORK_TYPE, 15);
    LOGI("%s: Set %s=%d", __func__,HOTSPOT_ACCESS_NETWORK_TYPE, 15);
}

bool
target_passpoint_configure(const struct schema_Passpoint_Config *conf, const char *if_name, bool state)
{
    bool ret = true;
    const char* filename = PASSPOINT_CONF_FILE_PATH_NAME(if_name);
    FILE *fptr;

    target_passpoint_init();

    LOGI("%s:  Entered", __func__);
    fptr = fopen(filename,"w");

    if(!fptr) {
        LOGE("%s: Could not open file for configuring passpoint %s", __func__, filename);
        return false;
    }

    set_hotspot_enable(fptr, state);
    set_hotspot_wan_metrics(fptr, conf);
    set_hotspot_hessid(fptr, conf);
    set_hotspot_domain_name(fptr, conf);
    set_hotspot_list_3gpp(fptr, conf);
    set_hotspot_nairealm(fptr, conf);
    set_hotspot_roaming_consortium(fptr, conf);
    set_hotspot_other(fptr, conf); // TODO, No configuration is coming for this from cloud
    set_hotspot_static(fptr);

    fclose(fptr);

    LOGI("%s:  Exited", __func__);
    return ret;
}

bool
target_passpoint_deconfigure(const char *if_name)
{
    bool ret = true;
    const char* filename = PASSPOINT_CONF_FILE_PATH_NAME(if_name);
    FILE *fptr;

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);
    // Set HOTSPOT_DISABLE
    fptr = fopen(filename,"w");

    if(!fptr) {
        LOGE("%s: Could not open file for decconfiguring passpoint %s", __func__, filename);
        ret = false;
    } else {
        fprintf(fptr,"%s=%d\n",HOTSPOT_ENABLE, 0);
        LOGI("%s: Hostapd Set to disable in passpoint config  file %s", __func__, filename);

        fclose(fptr);
        ret = true;
    }
    LOGI("%s:  Exited", __func__);
    return ret;
}

static bool
target_wait_on_Wifi_VIF_State_table_entry(char* if_name, bool enabled)
{
    bool ret = true;
    FILE *fp;
    char cmd[PWM_STRING_SIZE] = {0};
    char data[PWM_STRING_SIZE] = {0};

    if (enabled) {
        sprintf(cmd,"ovsh w Wifi_VIF_State -w if_name==%s enabled:=true -t %d",if_name, WAIT_TIMEOUT);
        fp = popen(cmd, "r");
        if (fp) {
            fgets(data, sizeof(data), fp);
        }
    } else {
        sprintf(cmd,"ovsh w Wifi_VIF_State -w if_name==%s -n enabled:=true -t %d",if_name, WAIT_TIMEOUT);
        fp = popen(cmd, "r");
        if (fp) {
            fgets(data, sizeof(data), fp);
        }
    }

    if(strncmp(data, "Error: timed out", strlen("Error: timed out")) ==0) {
        ret =false;
        LOGI("%s: WAIT for %s to get enbaled = %d in WIFI_VIF_State timed out", __func__, if_name, enabled);
    }

    pclose(fp);
    return ret;
}

/*
 * if vif state enable is true and hs20 is not 1 in hostapd config file
 * and  hs20 = 1 in passpoint config file then send trigger to recreate
 * VIF interface
 * In any other condition do nothing just return
 */
bool
target_passpoint_start(void)
{
    bool ret = true;
    DIR *d;
    char *value = NULL;
    struct dirent *dir;
    char vif_state_value[PWM_STRING_SIZE] = {0};

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);
    // get vif state for each interface
    d = opendir(CONFIG_PWM_PASSPOINT_CONFIG_DIR);
    if (!d) {
        LOGW("%s: Unable to open Passpoint configuration directory %s", __func__, CONFIG_PWM_PASSPOINT_CONFIG_DIR);
        goto close;
    }
    while ((dir = readdir(d)) != NULL) {
        if ( (strncmp(dir->d_name, ".", strlen(".")) == 0 ) || (strncmp(dir->d_name, "..", strlen("..")) == 0 ) ) {
            LOGD("%s: Skipping directory '%s'", __func__, dir->d_name);
            continue;
        }
        if (!get_value_ovsdb_table("Wifi_VIF_State", "if_name", dir->d_name, "enabled", vif_state_value)) {
            LOGW("%s: Cannot get enabled state of VIF %s", __func__, dir->d_name);
            continue;
        }
        if (strncmp(vif_state_value, "true", strlen("true")) != 0) {
            LOGD("%s: Skipping disabled VIF %s", __func__, dir->d_name);
            continue;
        }
        // Get value of Passpoint config file
        if ( !(value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                PASSPOINT_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT )) ||
                ((strncmp(value, "1", strlen("1"))) != 0) ) {
            LOGD("%s: Passpoint config not enabled for VIF %s", __func__, dir->d_name);
            continue;
        }
        // Skip to next VIF if config file is present and the hs20 is set enable.
        if ( (value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                HOSTAPD_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT )) &&
                ((strncmp(value, "1", strlen("1")) == 0)) ) {
            LOGW("%s: Passpoint Service already started for Interface %s", __func__, dir->d_name);
            continue;
        }
        //set enabled of Wifi_VIF_Config table to false then true in order for WM to apply the passpoint settings
        if (!set_value_ovsdb_table("Wifi_VIF_Config", "if_name", dir->d_name, "enabled", "false" )) {
            LOGW("%s: Not able to set enabled = false for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if (!target_wait_on_Wifi_VIF_State_table_entry(dir->d_name, false)) {
            LOGW("%s: Not able to set enabled = false for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if (!set_value_ovsdb_table("Wifi_VIF_Config", "if_name", dir->d_name, "enabled", "true" )) {
            LOGW("%s: Not able to set enabled = true for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if (!target_wait_on_Wifi_VIF_State_table_entry(dir->d_name, true)) {
            LOGW("%s: Not able to set enabled = true for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        LOGI("%s: %s interface %s Restarted", __func__, "Wifi_VIF_Config", dir->d_name);
    }

close:
    closedir(d);

    LOGI("%s:  Exited", __func__);
    return ret;
}

/*
 * if vif state enable is true and hs20 == 1 in hostapd config file
 * and  hs20 is not set in passpoint config file then send trigger to recreate
 * VIF interface
 * In any other condition do nothing just return
 */
bool
target_passpoint_stop(void)
{
    bool ret = true;
    DIR *d;
    char vif_state_value[PWM_STRING_SIZE] = {0};
    char *value = NULL;
    struct dirent *dir;

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);

    // get vif state for each interface
    d = opendir(CONFIG_PWM_PASSPOINT_CONFIG_DIR);
    if (!d) {
        LOGW("%s: Unable to open Passpoint configuration directory %s", __func__, CONFIG_PWM_PASSPOINT_CONFIG_DIR);
        goto close;
    }
    while ((dir = readdir(d)) != NULL) {
        if( (strncmp(dir->d_name, ".", strlen(".")) == 0 ) || (strncmp(dir->d_name, "..", strlen("..")) == 0 ) ) {
            LOGD("%s: Skipping directory '%s'", __func__, dir->d_name);
            continue;
        }
        LOGI("%s: Stopping Passpoint Service for vif if_name %s", __func__, dir->d_name);
        if (!get_value_ovsdb_table("Wifi_VIF_State", "if_name", dir->d_name, "enabled", vif_state_value)) {
            LOGW("%s: Cannot get enabled state of VIF %s", __func__, dir->d_name);
            continue;
        }
        if(strncmp(vif_state_value, "true", strlen("true")) != 0) {
            LOGD("%s: Skipping disabled VIF %s", __func__, dir->d_name);
            continue;
        }
        // Get value of hostapd config file
        if( !(value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                HOSTAPD_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT )) ||
                ((strncmp(value, "1", strlen("1"))) != 0) ) {
            // Passpoint service already stopped for Interface
            // as hs20 is not set to enable in hostapd config
            LOGW("%s: Passpoint service already stopped for Interface %s", __func__, dir->d_name);
            continue;
        }
        // Get value of passpoint config file to recreate vif
        // if hs20 is not set enable
        if( (value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                PASSPOINT_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT )) &&
                ((strncmp(value, "1", strlen("1")) == 0))) {
            continue;
        }
        //set enabled of Wifi_VIF_Config table to false then true in order for WM to apply the passpoint settings
        if (!set_value_ovsdb_table("Wifi_VIF_Config", "if_name", dir->d_name, "enabled", "false" )) {
            LOGW("%s: Not able to set enabled = false for Interface %s , can't Recreate", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if(!target_wait_on_Wifi_VIF_State_table_entry(dir->d_name, false)) {
            LOGW("%s: Not able to set enabled = false for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if (!set_value_ovsdb_table("Wifi_VIF_Config", "if_name", dir->d_name, "enabled", "true" )) {
            LOGW("%s: Not able to set enabled = true for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        if (!target_wait_on_Wifi_VIF_State_table_entry(dir->d_name, true)) {
            LOGW("%s: Not able to set enabled = false for Interface %s , can't Restart", __func__, dir->d_name);
            ret = false;
            continue;
        }
        LOGI("%s: %s interface %s Restarted and removed its passpoint config file = %s",
            __func__, "Wifi_VIF_Config", dir->d_name, PASSPOINT_CONF_FILE_PATH_NAME(dir->d_name));
        remove(PASSPOINT_CONF_FILE_PATH_NAME(dir->d_name));
    }

close:
    closedir(d);

    LOGI("%s:  Exited", __func__);
    return ret;
}

// get configuration of vif interface from its hostapd config file
// for parameter name
// Caller must free value returned
char
*target_passpoint_get_configuration(const char *name, const char *if_name)
{
    char *value = NULL;
    char *local_value = NULL;
    int max_params = PASSPOINT_PARAM_MAX_COUNT;

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);

    if (strcmp(name, CONFIG_PWM_PASSPOINT_ROAMING_CONSORTIUM) == 0)
        max_params = PASSPOINT_PARAM_ROAMING_CONSORTIUM_MAX_COUNT;

    local_value = get_value_from_config_file(if_name, name, HOSTAPD_CONF_FILE_PATH_NAME(if_name), max_params);
    LOGI("%s: %s = %s", __func__, name, local_value);

    if (local_value) {
        value = calloc(PWM_LONG_LONG_STRING_SIZE, sizeof(char));
        if ( value ) {
            memcpy(value, local_value, PWM_LONG_LONG_STRING_SIZE);
            LOGI("%s: Param = %s  Value %s", __func__, name, value);
        }
    }
    /* TODO to implement get value of other Passpoint_State table parameters
     * adv_wan_status, adv_wan_symmetric, adv_wan_at_capacity, other_config
     * Not implemented because no target API is defined for
     * adv_wan_status, adv_wan_symmetric, adv_wan_at_capacity
     * And valued of other_config to be set from  cloud are not known yet
     */
    LOGI("%s:  Exited", __func__);
    return value;
}

// get value of hs20 from passpoint config file for all vif interfaces and && them
// if value of all comes out to be true then it is true otherwise false
bool
target_passpoint_daemon_state(void)
{
    bool ret = false;
    DIR *d;
    char *value = NULL;
    struct dirent *dir;
    int hs20_forall = 1;

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);
    d = opendir(CONFIG_PWM_PASSPOINT_CONFIG_DIR);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if( (strncmp(dir->d_name, ".", strlen(".")) == 0 ) || (strncmp(dir->d_name, "..", strlen("..")) == 0 ) )
                continue;

            // Get value of hs20 from passpoint config file
            if((value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                    PASSPOINT_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT ))) {
                hs20_forall = hs20_forall & atoi(value);
                LOGI("%s: For Interface name %s value of hs20 =  %s", __func__,dir->d_name, value);
            } else
                hs20_forall = 0;

            if(!hs20_forall) {
                ret = false;
                break;
            } else
                ret = true;
        }
    }

    LOGI("%s:  Exited", __func__);
    return ret;
}

// get value of vif state enable and  hs20 from hostapd config file for  
// all vif interfaces and && them
// if value of all comes out to be true then it is true otherwise false
int
target_passpoint_get_service_state(void)
{
    bool ret = 0;
    DIR *d;
    char *value = NULL;
    struct dirent *dir;
    int hs20_forall = 1;
    char vif_state_value[PWM_STRING_SIZE] = {0};

    target_passpoint_init();
    LOGI("%s:  Entered", __func__);
    d = opendir(CONFIG_PWM_PASSPOINT_CONFIG_DIR);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if( (strncmp(dir->d_name, ".", strlen(".")) == 0 ) || (strncmp(dir->d_name, "..", strlen("..")) == 0 ) )
                continue;

            if (get_value_ovsdb_table("Wifi_VIF_State", "if_name", dir->d_name, "enabled", vif_state_value)  ) {
                if(strncmp(vif_state_value, "true", strlen("true")) != 0)
                    hs20_forall = 0;
            } else
                hs20_forall = 0;

            // Get value of hs20 from hostapd config file
            if(hs20_forall && (value = get_value_from_config_file(dir->d_name, HOTSPOT_ENABLE,
                    HOSTAPD_CONF_FILE_PATH_NAME(dir->d_name), PASSPOINT_PARAM_MAX_COUNT ))) {
                hs20_forall = hs20_forall & atoi(value);
                LOGI("%s: For Interface name %s value of hs20 =  %s", __func__,dir->d_name, value);
            } else
                hs20_forall = 0;

            if(!hs20_forall) {
                ret = 0;
                break;
            } else
                ret = 1;
        }
    }
    LOGI("%s:  Exited", __func__);
    return ret;
}
