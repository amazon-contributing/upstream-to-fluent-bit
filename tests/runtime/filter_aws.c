/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fluent-bit.h>
#include <fluent-bit/flb_info.h>
#include "flb_tests_runtime.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct aws_test_ctx {
    flb_ctx_t *flb;
};

struct aws_test_result {
    const char *target;
    const char *suffix;
    int   nMatched;
};

#define DPATH            FLB_TESTS_DATA_PATH "/data/aws"

// Helper function to clear the file
static void clear_file(const char *filename) {
    FILE *file;

    // Open the file in "w" mode to empty it
    file = fopen(filename, "w");
    if (file == NULL) {
        perror("Error opening file to clear content");
        return;
    }

    // Close the file to complete truncation
    fclose(file);
}

// Helper function to write to the file with the specified content
static void write_log_to_file(const char *filename) {
    FILE *file;
    char log_entry[512];

    // Log message to write
    const char *log_template = "{\"log\":\"Fluent Bit is logging\\n\",\"stream\":\"stdout\",\"time\":\"2019-04-01T17:58:33.598656444Z\"}";

    // Open the file for appending
    file = fopen(filename, "a");
    if (file == NULL) {
        perror("Error opening file");
        return;
    }
    // Format the final log entry with the current time
    snprintf(log_entry, sizeof(log_entry), log_template);

    // Write the log entry to the file
    fprintf(file, "%s\n", log_entry);

    // Close the file
    fclose(file);
}

static int file_to_buf(const char *path, char **out_buf, size_t *out_size)
{
    int ret;
    long bytes;
    char *buf;
    FILE *fp;
    struct stat st;

    ret = stat(path, &st);
    if (ret == -1) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    buf = flb_malloc(st.st_size);
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes != 1) {
        flb_errno();
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}

/* Given a target, lookup the .out file and return it content in a new buffer */
static char *get_out_file_content(const char *target, const char *suffix)
{
    int ret;
    char file[PATH_MAX];
    char *p;
    char *out_buf;
    size_t out_size;

    if (suffix) {
        snprintf(file, sizeof(file) - 1, DPATH "/out/%s_%s.out", target, suffix);
    }
    else {
        snprintf(file, sizeof(file) - 1, DPATH "/out/%s.out", target);
    }

    ret = file_to_buf(file, &out_buf, &out_size);
    TEST_CHECK_(ret == 0, "getting output file content: %s", file);
    if (ret != 0) {
        return NULL;
    }

    /* Sanitize content, get rid of ending \n */
    p = out_buf + (out_size - 1);
    while (*p == '\n' || *p == '\r') p--;
    *++p = '\0';

    return out_buf;
}

static int cb_check_result(void *record, size_t size, void *data)
{
    struct aws_test_result *result;
    char *out = NULL;

    result = (struct aws_test_result *) data;

        char *check;
        char streamfilter[64] = {'\0'};

        if (result->suffix && *result->suffix) {
            sprintf(streamfilter, "\"stream\":\"%s\"", result->suffix);
        }
        if (!*streamfilter ||
            strstr(record, streamfilter)) {
            out = get_out_file_content(result->target, result->suffix);
            if (!out) {
                goto exit;
            }
            /*
             * Our validation is: check that the content of out file is found
             * in the output record.
             */
            check = strstr(record, out);
            TEST_CHECK_(check != NULL,
                       "comparing expected record with actual record");
            if (check == NULL) {
                if (result->suffix) {
                    printf("Target: %s, suffix: %s\n",
                           result->target, result->suffix);
                }
                else
                {
                    printf("Target: %s\n",
                           result->target);
                }
                printf("Expected record:\n%s\n"
                       "Actual record:\n%s\n",
                       out, (char *)record);
            }
            result->nMatched++;
        }

exit:
    if (size > 0) {
        flb_free(record);
    }
    if (out) {
        flb_free(out);
    }
    return 0;
}

static void aws_test(const char *target, const char *suffix, int nExpected, ...)
{
    int ret;
    int in_ffd;
    int filter_ffd;
    int out_ffd;
    char *key;
    char *value;
    char path[PATH_MAX];
    va_list va;
    struct aws_test_ctx ctx;
    struct flb_lib_out_cb cb_data;
    struct aws_test_result result = {0};

    result.nMatched = 0;
    result.target = target;
    result.suffix = suffix;

    ctx.flb = flb_create();
    TEST_CHECK_(ctx.flb != NULL, "initialising service");
    if (!ctx.flb) {
        goto exit;
    }

    ret = flb_service_set(ctx.flb,
                          "Flush", "1",
                          "Grace", "1",
                          "Log_Level", "debug",
                          "Parsers_File", DPATH "/parsers.conf",
                          NULL);
    TEST_CHECK_(ret == 0, "setting service options");

        /* Compose path based on target */
        snprintf(path, sizeof(path) - 1, DPATH "/log/%s.log", target);
        TEST_CHECK_(access(path, R_OK) == 0, "accessing log file: %s", path);
        in_ffd = flb_input(ctx.flb, "tail", NULL);
        TEST_CHECK_(in_ffd >= 0, "initialising input");
        ret = flb_input_set(ctx.flb, in_ffd,
                            "Tag", "host.dmesg",
                            "Path", path,
                            "Parser", "docker",
                            "Docker_Mode", "On",
                            "read_from_head", "on",
                            NULL);
        TEST_CHECK_(ret == 0, "setting input options");

    filter_ffd = flb_filter(ctx.flb, "aws", NULL);
    TEST_CHECK_(filter_ffd >= 0, "initialising filter");
    //change this
    ret = flb_filter_set(ctx.flb, filter_ffd,
                        "imds_version", "v2",
                        "Match", "host.*",
                         NULL);
    TEST_CHECK_(ret == 0, "setting filter options");

    /* Iterate number of arguments for filter_aws additional options */
    va_start(va, nExpected);
    while ((key = va_arg(va, char *))) {
        value = va_arg(va, char *);
        if (!value) {
            /* Wrong parameter */
            break;
        }
        ret = flb_filter_set(ctx.flb, filter_ffd, key, value, NULL);
        TEST_CHECK_(ret == 0, "setting filter additional options");
    }
    va_end(va);
    
    /* Prepare output callback context*/
    cb_data.cb = cb_check_result;
    cb_data.data = &result;

    /* Output */
    out_ffd = flb_output(ctx.flb, "lib", (void *) &cb_data);
    TEST_CHECK_(out_ffd >= 0, "initialising output");
    flb_output_set(ctx.flb, out_ffd,
                   "Match", "host.*",
                   "format", "json",
                   NULL);
    TEST_CHECK_(ret == 0, "setting output options");

    clear_file(path);

    //Testing the default values setup
    struct mk_list *head;
    struct flb_filter_instance *f_ins;
    mk_list_foreach(head, &ctx.flb->config->filters) {
        f_ins = mk_list_entry(head, struct flb_filter_instance, _head);
        if (strstr(f_ins->p->name, "aws")) {
            TEST_CHECK_(strcmp(f_ins->p->config_map[9].name, "enable_entity") == 0, "checking the enable entity field in filter config map");
            TEST_CHECK(strcmp(f_ins->p->config_map[9].def_value, "false") == 0);
            TEST_CHECK_(strcmp(f_ins->p->config_map[10].name, "entity_type") == 0, "checking the enable entity field in filter config map");
            TEST_CHECK(strcmp(f_ins->p->config_map[10].def_value, "service") == 0);
        }
    }

    /* Start the engine */
    ret = flb_start(ctx.flb);
    TEST_CHECK_(ret == 0, "starting engine");
    if (ret == -1) {
        goto exit;
    }

    /* Poll for up to 3 seconds or until we got a match */
    for (ret = 0; ret < 3000 && result.nMatched != nExpected; ret++) {
        usleep(1000);
        if (ret == 2000) {
            write_log_to_file(path);
        }
    }
    TEST_CHECK(result.nMatched == nExpected);
    TEST_MSG("result.nMatched: %i\nnExpected: %i", result.nMatched, nExpected);

    ret = flb_stop(ctx.flb);
    TEST_CHECK_(ret == 0, "stopping engine");

exit:
    if (ctx.flb) {
        flb_destroy(ctx.flb);
    }
}

static void flb_test_aws_success()
{
    aws_test("options_default", NULL, 1, \
                NULL);
}

static void flb_test_aws_entity()
{
    aws_test("options_resource_entity", NULL, 1, \
                "entity_type", "resource", \
                "enable_entity", "On", \
                "set_platform", "eks", \
                NULL);
}

TEST_LIST = {
    {"aws_options_generic", flb_test_aws_success },
    //{"aws_options_resource_entity", flb_test_aws_entity},
    {NULL, NULL}
};
