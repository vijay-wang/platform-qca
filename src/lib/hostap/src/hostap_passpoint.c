#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "const.h"
#include "log.h"
#include "kconfig.h"
#include "target.h"

#ifdef CONFIG_MANAGER_PWM
/*
 * @brief Read Passpoint configuration from file for if_name
 * and save the configuration in buf
 */
static void
hapd_passpoint_conf_gen(const char *if_name, char **buf, size_t *len)
{
    FILE *fptr = NULL;
    char filename[32] = {0};
    char *file_line = NULL;
    #define FILE_LINE_SIZE 512

    sprintf(filename,"%s/%s", CONFIG_PASSPOINT_CONFIG_DIR, if_name);
    fptr = fopen(filename,"r");

    if(fptr == NULL) {
        LOGE("Passpoint config file does not exist for ifname = %s", if_name);
        return;
    }

    file_line = calloc(FILE_LINE_SIZE, sizeof(char));
    if (file_line == NULL) {
        LOGE("Malloc Failed for file_line");
        return;
    }

    while (fgets(file_line, FILE_LINE_SIZE, fptr) != NULL ) {

        csnprintf(buf, len, "%s", file_line);
        memset(file_line, 0, FILE_LINE_SIZE);
    }
    LOGI(" %s : Passpoint configuration written into hostapd config file for Interface = %s",__func__, if_name);

    if(file_line) {
       free(file_line);
       file_line = NULL;
    }

    fclose(fptr);
}
#endif
