/*
 * Copyright © 2016-2018 Inria.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 * See COPYING in top-level directory.
 *
 * $HEADER$
 */

#define _GNU_SOURCE // for asprintf
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <libgen.h> // for dirname
#include <sys/types.h>
#include <dirent.h>
#include <regex.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "include/netloc-utils.h"
#include "include/netloc-wip.h"
#include "include/netloc-datatypes.h"

static int read_routes(route_source_t **proutes, char *subnet, char *path,
        char *route_dirname);
static int read_discover(char *subnet, char *discover_path,
                         char *filename, utils_node_t **nodes);

static void
get_match(char *line, int nmatch, regmatch_t pmatch[], char *matches[])
{
    for (int i = 0; i < nmatch; i++) {
        regmatch_t current_match = pmatch[i];
        size_t len = current_match.rm_eo-current_match.rm_so;
        matches[i] = (char *)malloc((len+1)*sizeof(char));
        strncpy(matches[i], line+current_match.rm_so, len);
        matches[i][len] = '\0';
    }
}

/* We suppose the description of nodes is like that: ([^ ]*).*
 * while \1 is the hostname
 */
static char *node_find_hostname(utils_node_t *node)
{
    char *name = node->description;
    int max_size = strlen(name);
    char *hostname = (char *)malloc(max_size*sizeof(char));

    /* Looking for the name of the hostname */
    int i = 0;
    if (name[0] == '\'')
        name++;
    while (i < max_size &&
            ((name[i] >= 'a' && name[i] <= 'z') ||
             (name[i] >= '0' && name[i] <= '9') ||
             (name[i] == '-'))) {
        hostname[i] = name[i];
        i++;
    }
    hostname[i++] = '\0';
    char *old_hostname = hostname;
    hostname = realloc(hostname, i*sizeof(char));
    if (!hostname) {
        fprintf(stderr, "ERROR: cannot reallocate memory\n");
        hostname = old_hostname;
    }
    return hostname;
}

utils_node_t *get_node(utils_node_t **nodes, char *type, char *lid,
        char *guid, char *subnet, char *desc)
{
    static unsigned unknown_node_id = 0;
    utils_node_t *node;
    char *id;

    asprintf(&id, "%.4s:%.4s:%.4s:%.4s",
            guid, guid+4, guid+8, guid+12);

    // TODO check guid format
    HASH_FIND_STR(*nodes, id, node);  /* id already in the hash? */
    if (!node) {
        size_t size = sizeof(*node)+sizeof(char)*(strlen(desc)+1);
        node = (utils_node_t *) malloc(size);
        sprintf(node->physical_id, "%s", id);

        node->logical_id = atol(lid);
        node->type = netloc_node_type_decode(type);
        node->edges = NULL;
        node->description = strdup(desc);
        node->hostname = node_find_hostname(node);
        if (NETLOC_NODE_TYPE_HOST == node->type
            && 0 == strlen(node->hostname)) {
            free(node->hostname);
            asprintf(&node->hostname, "ANONYMOUS-%u", unknown_node_id);
            unknown_node_id += 1;
            assert(NULL != node->hostname);
        }
        node->main_partition = -1;
        node->partitions = NULL;
        node->subnodes = NULL;

        utarray_init(&node->physical_links, &utils_physical_link_icd);

        HASH_ADD_STR(*nodes, physical_id, node);  /* guid: name of key field */
    }
    free(id);

    return node;
}

static utils_physical_link_t *
find_other_physical_link(utils_physical_link_t *link)
{
    utils_node_t *dest = link->dest;
    unsigned int dest_port = link->ports[1];

    utils_physical_link_t *other_link = (utils_physical_link_t *)
        utarray_eltptr(&dest->physical_links, dest_port-1);

    return other_link;
}

static float compute_gbits(char *speed, char *width)
{
    float rate;
    float encoding;
    float gb_per_x;
    int x;

    if (strcmp(speed, "SDR")) {
        rate = 2.5;
        encoding = 8.0/10;
    } else if (strcmp(speed, "DDR")) {
        rate = 5;
        encoding = 8.0/10;
    } else if (strcmp(speed, "QDR")) {
        rate = 10;
        encoding = 8.0/10;
    } else if (strcmp(speed, "FDR")) {
        rate = 14.0625;
        encoding = 64.0/66;
    } else if (strcmp(speed, "FDR10")) {
        rate = 10;
        encoding = 64.0/66;
    } else if (strcmp(speed, "EDR")) {
        rate = 25;
        encoding = 64.0/66;
    } else {
        return 1;
    }
    gb_per_x = rate*encoding;

    regex_t width_re;
    regcomp(&width_re, "([[:digit:]]*)x", REG_EXTENDED);
    if (!regexec(&width_re, width, (size_t)0, NULL, 0)) {
        x = atoi(width);
    } else {
        x = 0.0;
    }

    regfree(&width_re);

    return x*gb_per_x;
}

int build_paths(path_source_t **ppaths, utils_node_t *nodes, route_source_t *routes)
{
    utils_node_t *node_src, *node_dest, *node_tmp1, *node_tmp2;
    path_source_t *paths = *ppaths;
    HASH_ITER(hh, nodes, node_src, node_tmp1) {
        if (node_src->type != NETLOC_NODE_TYPE_HOST)
            continue;

        if (0 == HASH_COUNT(node_src->edges))
            continue;

        char *id_src = node_src->physical_id;

        path_source_t *path = (path_source_t *)
            malloc(sizeof(path_source_t));
        strncpy(path->physical_id, id_src, MAX_STR);
        path->physical_id[MAX_STR-1] = '\0';
        path->node = node_src;
        path->dest = NULL;
        HASH_ADD_STR(paths, physical_id, path);

        HASH_ITER(hh, nodes, node_dest, node_tmp2) {
            if (node_dest->type != NETLOC_NODE_TYPE_HOST)
                continue;

            if (node_dest == node_src)
                continue;

            UT_array found_links;
            utarray_init(&found_links, &ut_ptr_icd);
            int completed = 1;

            char *id_dest = node_dest->physical_id;

            /* Don't assert node_src->physical_link[0] is set */
            unsigned int link_id = *(int *)
                utarray_front(&node_src->edges->physical_link_idx);
            utils_physical_link_t *link = (utils_physical_link_t  *)
                utarray_eltptr(&node_src->physical_links, link_id);
            utarray_push_back(&found_links, &link);

            assert(link);
            utils_node_t *node_cur = link->dest;
            assert(node_cur);
            while (node_cur != node_dest) {
                route_source_t *route_source;
                route_dest_t *route_dest;
                char *id_cur = node_cur->physical_id;
                HASH_FIND_STR(routes, id_cur, route_source);
                if (!route_source) {
                    completed = 0;
                    break;
                }
                HASH_FIND_STR(route_source->dest, id_dest, route_dest);
                if (!route_dest) {
                    completed = 0;
                    break;
                }

                unsigned int port = route_dest->port;
                link = (utils_physical_link_t *)
                    utarray_eltptr(&node_cur->physical_links, port-1);
                utarray_push_back(&found_links, &link);
                assert(link);
                node_cur = link->dest;
            }

            if (completed) {
                path_dest_t *path_dest = (path_dest_t *)
                    malloc(sizeof(path_dest_t));
                strncpy(path_dest->physical_id, id_dest, MAX_STR);
                path_dest->physical_id[MAX_STR-1] = '\0';
                path_dest->node = node_dest;
                path_dest->links = found_links;
                HASH_ADD_STR(path->dest, physical_id, path_dest);
            } else {
                utarray_done(&found_links);
            }
        }
    }
    *ppaths = paths;
    return 0;
}

static inline int proper_partition_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* We suppose the hostname of nodes is like that: ([a-z][-a-z]+[a-z]).*
 * while \1 is the name of the partition
 */
static char *node_find_partition_name(utils_node_t *node)
{
    char *name;
    int max_size;
    char *partition;

    max_size = strlen(node->hostname);
    partition = (char *)malloc(sizeof(char[max_size+1]));
    name = node->hostname;

    /* Looking for the name of the partition */
    int i = 0;
    while (i < max_size && proper_partition_name_char(name[i])) {
        partition[i] = name[i];
        i++;
    }
    while (i > 0 && partition[i-1] == '-') {
        partition[i-1] = '\0';
        --i;
    }
    partition[i++] = '\0';

    char *old_partition = partition;
    partition = realloc(partition, sizeof(char[i]));
    if (!partition) {
        fprintf(stderr, "ERROR: cannot reallocate memory\n");
        partition = old_partition;
    }
    return partition;
}


static int
netloc_network_explicit_find_partitions(utils_node_t *nodes,
                                        UT_array *partitions)
{
    int ret = NETLOC_SUCCESS;;
    int num_nodes;
    utils_partition_t **partition_names;
    utils_node_t **hosts;

    num_nodes = HASH_COUNT(nodes);
    partition_names = (utils_partition_t **)
        malloc(sizeof(utils_partition_t *[num_nodes]));
    hosts = (utils_node_t **)malloc(sizeof(utils_node_t *[num_nodes]));

    /* Save all the partition names */
    int n = 0;
    utils_node_t *node, *node_tmp;
    HASH_ITER(hh, nodes, node, node_tmp) {
        if (node->type != NETLOC_NODE_TYPE_HOST)
            continue;
        partition_names[n] = (utils_partition_t *)
            malloc(sizeof(utils_partition_t));
        partition_names[n]->name = node_find_partition_name(node);
        utarray_init(&partition_names[n]->nodes, &ut_ptr_icd);
        utarray_push_back(&partition_names[n]->nodes, &node);
        hosts[n] = node;
        n++;
    }

    /* Associate the field partition in the nodes to the correct partition
     * index
     */
    int num_hosts = n;
    int num_partitions = 0;
    for (int n1 = 0; n1 < num_hosts; n1++) {
        if (!partition_names[n1])
            continue;
        partition_names[num_partitions] = partition_names[n1];
        hosts[n1]->main_partition = num_partitions;

        /* Ensure each partition name is unique in the array */
        for (int n2 = n1+1; n2 < num_hosts; n2++) {
            if (!partition_names[n2])
                continue;

            if (!strcmp(partition_names[n1]->name, partition_names[n2]->name)) {
                utarray_concat(&partition_names[n1]->nodes,
                               &partition_names[n2]->nodes);
                free(partition_names[n2]->name);
                utarray_done(&partition_names[n2]->nodes);
                free(partition_names[n2]);
                partition_names[n2] = NULL;
                hosts[n2]->main_partition = num_partitions;
            }
        }
        num_partitions++;
    }

    printf("%d partitions found\n", num_partitions);
    utarray_reserve(partitions, num_partitions);
    for (int p = 0; p < num_partitions; p++) {
        printf("\t'%s'\n", partition_names[p]->name);
        utarray_push_back(partitions, partition_names+p);
    }
    free(partition_names);
    free(hosts);

    return ret;
}

static int
netloc_network_explicit_set_partitions(utils_node_t *nodes,
        UT_array *partitions, path_source_t *paths)
{
    /* Find the main partition for each node */
    netloc_network_explicit_find_partitions(nodes, partitions);

    utils_node_t *node, *node_tmp;
    HASH_ITER(hh, nodes, node, node_tmp) {
        node->partitions = (int *)
            calloc(utarray_len(partitions), sizeof(int));
        if (node->main_partition != -1)
            node->partitions[node->main_partition] = 1;

        utils_edge_t *edge, *edge_tmp;
        HASH_ITER(hh, node->edges, edge, edge_tmp) {
            edge->partitions = (int *)
                calloc(utarray_len(partitions), sizeof(int));
        }
    }

    /* Set the partitions for the physical links considering if there is in a
     * path between two nodes of a partition */
    path_source_t *path_src, *path_src_tmp;
    HASH_ITER(hh, paths, path_src, path_src_tmp) {
        utils_node_t *node_src = path_src->node;
        int partition = node_src->main_partition;
        path_dest_t *path_dest, *path_dest_tmp;
        HASH_ITER(hh, path_src->dest, path_dest, path_dest_tmp) {
            utils_node_t *node_dest = path_dest->node;
            if (node_dest->main_partition != partition)
                continue;

            for (unsigned int l = 0; l < utarray_len(&path_dest->links); l++) {
                utils_physical_link_t *link = *(utils_physical_link_t **)
                    utarray_eltptr(&path_dest->links, l);
                if (!link->partitions) {
                    link->partitions = (int *)
                        calloc(utarray_len(partitions), sizeof(int));
                }
                link->partitions[partition] = 1;
                link->parent_node->partitions[partition] = 1;
                link->parent_edge->partitions[partition] = 1;
                /* Set reverse physical_link */
                link = link->other_link;
                if (!link) {
                    continue;
                } else if (!link->partitions) {
                    link->partitions = (int *)
                        calloc(utarray_len(partitions), sizeof(int));
                }
                link->partitions[partition] = 1;
                link->parent_node->partitions[partition] = 1;
                link->parent_edge->partitions[partition] = 1;
            }
        }
    }
    return NETLOC_SUCCESS;
}

void help(char *name, FILE *f)
{
    fprintf(f, "Usage: %s <path to input raw data files> <output path> "
            "[--hwloc-dir <hwloc xml path>]\n"
            "\thwloc-dir can be an absolute path "
            "or a relative path from out-dir\n", name);
}

int main(int argc, char **argv)
{
    DIR *indir, *outdir;
    char *prog_name = basename(argv[0]);
    char *inpath = NULL, *outpath = NULL, *hwlocpath = NULL;
    utils_node_t *nodes = NULL;
    route_source_t *routes = NULL;
    path_source_t *paths = NULL;

    UT_array partitions;

    if (argc != 2 && argc != 3 && argc != 5) {
        goto error_param;
    }
    argc--; argv++;

    if (!strcmp(*argv, "--help")) {
        help(prog_name, stdout);
        return 0;
    }

    inpath = *argv;
    argc--; argv++;

    if (!argc)
        goto error_param;
    outpath = *argv;
    argc--; argv++;

    while (argc > 0) {
        if (!strcmp(*argv, "--hwloc-dir")) {
            argc--; argv++;
            if (!argc)
                goto error_param;
            hwlocpath = *argv;
        } else if (!strcmp(*argv, "--help")) {
            help(prog_name, stdout);
            return 0;
        } else {
            goto error_param;
        }
        argc--;
        argv++;
    }

    if (!outpath || !inpath) {
        goto error_param;
    }

    indir = opendir(inpath);
    if (!indir) {
        fprintf(stderr, "Couldn't open input directory: \"%s\"\n", inpath);
        perror("opendir");
        return 2;
    }

    outdir = opendir(outpath);
    if (!outdir) {
        fprintf(stderr, "Couldn't open output directory: \"%s\"\n", outpath);
        perror("opendir");
        return 2;
    }

    if (hwlocpath) {
        char *realpath;
        if (hwlocpath[0] != '/') {
            asprintf(&realpath, "%s/%s", outpath, hwlocpath);
        } else {
            realpath = strdup(hwlocpath);
        }

        DIR *hwlocdir = opendir(realpath);
        if (!hwlocdir) {
            fprintf(stderr, "Couldn't open hwloc directory: \"%s\"\n", realpath);
            perror("opendir");
            closedir(indir);
            closedir(outdir);
            free(realpath);
            return 2;
        }
        free(realpath);
        closedir(hwlocdir);
    }

    regex_t subnet_regexp;
    regcomp(&subnet_regexp, "^ib-subnet-([0-9a-fA-F:]{19}).txt$", REG_EXTENDED);
    struct dirent *entry;
    while ((entry = readdir(indir))) {
        nodes = NULL;
        utarray_init(&partitions, &ut_ptr_icd);
        int subnet_found;
        char *filename = entry->d_name;

        subnet_found = !(regexec(&subnet_regexp, filename, 0, NULL, 0));
        if (subnet_found) {
            char *discover_filename;
            char *route_filename;
            char *subnet;
            if (0 > asprintf(&subnet, "%.19s", filename+10))
                subnet = NULL;

            discover_filename = filename;
            read_discover(subnet, inpath, discover_filename, &nodes);

            asprintf(&route_filename, "%s/ibroutes-%s", inpath, subnet);

            struct stat s;
            int err = stat(route_filename, &s);
            if (-1 == err) {
                if (errno == ENOENT) {
                    printf("No route directory found for subnet %s\n", subnet);
                } else {
                    perror("stat");
                    exit(1);
                }
            } else {
                if (S_ISDIR(s.st_mode)) {
                    char *route_filename;
                    if (0 > asprintf(&route_filename, "ibroutes-%s", subnet))
                        route_filename = NULL;
                    read_routes(&routes, subnet, inpath, route_filename);
                    free(route_filename);
                } else {
                    printf("No route directory found for subnet %s\n", subnet);
                }
            }
            free(route_filename);

            build_paths(&paths, nodes, routes);
            netloc_network_explicit_set_partitions(nodes, &partitions, paths);

            set_reverse_edges(nodes);
            find_similar_nodes(&nodes, utarray_len(&partitions));

            /* Write the XML file */
            netloc_machine_t *machine =
                netloc_machine_construct(subnet);


            utils_to_netloc_machine(machine, nodes, &partitions, subnet, outpath,
                                              hwlocpath,
                                              NETLOC_NETWORK_TYPE_INFINIBAND);

            netloc_arch_build(machine);
            netloc_machine_to_xml(machine);

            /* Free node hash table */
            utils_node_t *node, *node_tmp;
            HASH_ITER(hh, nodes, node, node_tmp) {
                HASH_DEL(nodes, node);

                /* Free nodes */
                free(node->description);

                /* Edges */
                utils_edge_t *edge, *edge_tmp;
                HASH_ITER(hh, node->edges, edge, edge_tmp) {
                    HASH_DEL(node->edges, edge);
                    utarray_done(&edge->physical_link_idx);
                    free(edge->partitions);
                    free(edge);
                }

                free(node->hostname);
                free(node->partitions);

                /* Physical links */
                for (unsigned l=0; l < utarray_len(&node->physical_links); l++) {
                    utils_physical_link_t *link = (utils_physical_link_t *)
                        utarray_eltptr(&node->physical_links, l);
                    free(link->width);
                    free(link->speed);
                    free(link->description);
                    free(link->partitions);
                }
                utarray_done(&node->physical_links);

                free(node);
            }

            /* Free Partitions */
            for (utils_partition_t **ppartition =
                     (utils_partition_t **) utarray_front(&partitions);
                 ppartition != NULL;
                 ppartition = (utils_partition_t **)
                     utarray_next(&partitions, ppartition)) {
                free((*ppartition)->name);
                utarray_done(&(*ppartition)->nodes);
                free(*ppartition);
            }
            utarray_done(&partitions);

            /* Free Routes */
            route_source_t *route, *route_tmp;
            HASH_ITER(hh, routes, route, route_tmp) {
                HASH_DEL(routes, route);

                route_dest_t *routed, *routed_tmp;
                HASH_ITER(hh, route->dest, routed, routed_tmp) {
                    HASH_DEL(route->dest, routed);
                    free(routed);
                }
                free(route);
            }

            /* Free Paths */
            path_source_t *path, *path_tmp;
            HASH_ITER(hh, paths, path, path_tmp) {
                HASH_DEL(paths, path);

                path_dest_t *pathd, *pathd_tmp;
                HASH_ITER(hh, path->dest, pathd, pathd_tmp) {
                    HASH_DEL(path->dest, pathd);
                    utarray_done(&pathd->links);
                    free(pathd);
                }
                free(path);
            }

            free(subnet);
        }
    }
    regfree(&subnet_regexp);
    closedir(indir);
    closedir(outdir);

    return 0;

error_param:
    fprintf(stderr, "Wrong parameters\n");
    help(prog_name, stderr);
    return 1;
}

int read_discover(char *subnet, char *path,
        char *filename, utils_node_t **nodes)
{
    char *line = NULL;
    size_t size = 0;
    char *discover_path;
    FILE *discover_file;

    if (0 > asprintf(&discover_path, "%s/%s", path, filename))
        discover_path = NULL;
    discover_file = fopen(discover_path, "r");
    free(discover_path);

    if (!discover_file) {
        perror("fopen");
        exit(-1);
    }

    regex_t dr_re;
    regcomp(&dr_re, "^DR", REG_EXTENDED);

    regex_t link_re;
    regcomp(&link_re, "^"
            "(CA|SW)[[:space:]]+"                // Source type
            "([[:digit:]]+)[[:space:]]+"         // Source lid
            "([[:digit:]]+)[[:space:]]+"         // Source port id
            "0x([0-9a-f]{16})[[:space:]]+"       // Source guid
            "([[:digit:]]+x)[[:space:]]"         // Connection width
            "([^[:space:]]*)[[:space:]]+"        // Connection speed
            "-[[:space:]]+"                      // Dash seperator
            "(CA|SW)[[:space:]]+"                // Dest type
            "([[:digit:]]+)[[:space:]]+"         // Dest lid
            "([[:digit:]]+)[[:space:]]+"         // Dest port id
            "0x([0-9a-f]{16})[[:space:]]+"       // Dest guid
            "\\([[:space:]]*(.*)[[:space:]]*\\)" // Description
            ,
            REG_EXTENDED);

    regex_t nolink_re;
    regcomp(&nolink_re, "^"
            "(CA|SW)[[:space:]]+"          // Source type
            "([[:digit:]]+)[[:space:]]+"   // Source lid
            "([[:digit:]]+)[[:space:]]+"   // Source port id
            "0x([0-9a-f]{16})[[:space:]]+"       // Source guid
            ,
            REG_EXTENDED);

    int read;
    unsigned long long int global_link_idx = 0;
    errno = 0; /* getline can return -1 even if no error (EOF) */
    while ((read = getline(&line, &size, discover_file)) > 0) {
        const int link_nfields = 12;
        const int nolink_nfields = 7;
        const int max_nfields = 12;
        regmatch_t pmatch[max_nfields];
        char *matches[max_nfields];
        char *src_type;
        char *src_lid;
        char *src_port_id;
        char *src_guid;
        char *width;
        char *speed;
        char *dest_type;
        char *dest_lid;
        char *dest_port_id;
        char *dest_guid;
        char *link_desc;
        char *src_desc = NULL;
        char *dest_desc = NULL;
        int have_peer;

        if (!regexec(&dr_re, line, (size_t)0, NULL, 0)) {
            /* DR line */
            continue;
        }
        else if (!regexec(&link_re, line, (size_t)link_nfields, pmatch, 0)) {
            /* peer associated: port is active */
            have_peer = 1;
            get_match(line, link_nfields, pmatch, matches);
            src_type     = matches[ 1];
            src_lid      = matches[ 2];
            src_port_id  = matches[ 3];
            src_guid     = matches[ 4];
            width        = matches[ 5];
            speed        = matches[ 6];
            dest_type    = matches[ 7];
            dest_lid     = matches[ 8];
            dest_port_id = matches[ 9];
            dest_guid    = matches[10];
            link_desc    = matches[11];
            free(matches[0]);

            /* Analyse description */
            regex_t desc_re;
            regcomp(&desc_re, "(.*)" "[[:space:]]+-[[:space:]]+" "(.*)", REG_EXTENDED);
            if (!regexec(&desc_re, link_desc, (size_t)3, pmatch, 0)) {
                get_match(link_desc, 3, pmatch, matches);
                src_desc  = matches[1];
                dest_desc = matches[2];
                free(matches[0]);
            }
            else {
                src_desc = (char *)calloc(1, sizeof(char));
                dest_desc = (char *)calloc(1, sizeof(char));
            }
            regfree(&desc_re);

        }
        else if (!regexec(&nolink_re, line, (size_t)nolink_nfields, pmatch, 0)) {
            /* no peer associated: port is not active */
            have_peer = 0;
            get_match(line, nolink_nfields, pmatch, matches);
            src_type     = matches[ 1];
            src_lid      = matches[ 2];
            src_port_id  = matches[ 3];
            src_guid     = matches[ 4];
            width        = matches[ 5];
            speed        = matches[ 6];
            free(matches[0]);
        }
        else {
            printf("Warning: line not recognized: \n\t%s\n", line);
            continue;
        }

        /* Compute gbits */
        float gbits = compute_gbits(speed, width);

        /* Add the link to the edge list */
        if (have_peer) {
            /* Get the source node */
            utils_node_t *src_node =
                get_node(nodes, src_type, src_lid, src_guid, subnet, src_desc);

            utils_node_t *dest_node =
                get_node(nodes, dest_type, dest_lid, dest_guid, subnet, dest_desc);

            utils_edge_t *edge;
            HASH_FIND_STR(src_node->edges, dest_node->physical_id, edge);
            /* Creation of the edge */
            if (!edge) {
                edge = (utils_edge_t *) malloc(sizeof(utils_edge_t));
                strcpy(edge->dest, dest_node->physical_id);
                edge->total_gbits = 0;
                edge->partitions = NULL;
                edge->reverse_edge = NULL;
                utarray_init(&edge->subedges, &ut_ptr_icd);
                utarray_init(&edge->physical_link_idx, &ut_int_icd);
                HASH_ADD_STR(src_node->edges, dest, edge);
            }

            /* Creation of the physical link */
            utils_physical_link_t link[1];
            link->int_id = global_link_idx++;
            link->ports[0] = atoi(src_port_id);
            link->ports[1] = atoi(dest_port_id);
            link->width = strdup(width);
            link->speed = strdup(speed);
            link->gbits = gbits;
            link->dest = dest_node;
            edge->total_gbits += gbits;
            link->description = strdup(link_desc);
            link->partitions = NULL;
            link->parent_edge = edge;
            link->parent_node = src_node;
            link->other_link = NULL;

            unsigned int port_idx = link->ports[0]-1;
            /* NB: there is no function to set a specific index */
            if (port_idx+1 > utarray_len(&src_node->physical_links)) {
                utarray_insert(&src_node->physical_links, link, port_idx);
            } else {
                utils_physical_link_t *dest_link = (utils_physical_link_t *)
                    utarray_eltptr(&src_node->physical_links, port_idx);
                memcpy(dest_link, link, sizeof(utils_physical_link_t));
            }

            utarray_push_back(&edge->physical_link_idx, &port_idx);

            free(src_desc);
            free(dest_desc);
            free(dest_type);
            free(dest_lid);
            free(dest_port_id);
            free(dest_guid);
            free(link_desc);
        }

        free(src_type);
        free(src_lid);
        free(src_port_id);
        free(src_guid);
        free(width);
        free(speed);
    }

    int failed = 0;
    if (read == -1 && errno) {
        perror("getline:");
        failed = 1;
    }

    free(line);
    regfree(&dr_re);
    regfree(&link_re);
    regfree(&nolink_re);
    fclose(discover_file);

    if (failed)
        exit(-1);


    /* Find the link in the other way */
    utils_node_t *node, *node_tmp;
    HASH_ITER(hh, *nodes, node, node_tmp) {
        if (node->subnodes) {
            utils_node_t*subnode, *subnode_tmp;
            HASH_ITER(hh, node->subnodes, subnode, subnode_tmp) {
                unsigned int num_links = utarray_len(&subnode->physical_links);
                for (unsigned int i = 0; i < num_links; i++) {
                    utils_physical_link_t *link = (utils_physical_link_t *)
                        utarray_eltptr(&subnode->physical_links, i);
                    if (!link->dest)
                        continue;
                    link->other_link = find_other_physical_link(link);
                }
            }
        } else {
            unsigned int num_links = utarray_len(&node->physical_links);
            for (unsigned int i = 0; i < num_links; i++) {
                utils_physical_link_t *link = (utils_physical_link_t *)
                    utarray_eltptr(&node->physical_links, i);
                if (!link->dest)
                    continue;
                link->other_link = find_other_physical_link(link);
            }
        }
    }

    return 0;
}

int read_routes(route_source_t **proutes, char *subnet, char *path, char *route_dirname)
{
    char *route_path;
    route_source_t *routes = *proutes;
    DIR *dir;

    if (0 > asprintf(&route_path, "%s/%s", path, route_dirname))
        route_path = NULL;
    dir = opendir(route_path);

    printf("Read subnet: %s\n", subnet);

    if (dir != NULL) {
        char *line = NULL;
        size_t size = 0;
        regex_t route_filename_regexp;
        regcomp(&route_filename_regexp, "^ibroute-[0-9a-fA-F:]{19}-([0-9]*).txt$", REG_EXTENDED);
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            char *filename = entry->d_name;

            if (!(regexec(&route_filename_regexp, filename, 0, NULL, 0))) {
                char *route_filename;
                if (0 > asprintf(&route_filename, "%s/%s", route_path, filename))
                    route_filename = NULL;

                FILE *route_file = fopen(route_filename, "r");
                if (!route_file) {
                    perror("fopen");
                    exit(-1);
                }

                free(route_filename);

                regex_t header_re;
                regcomp(&header_re, "^Unicast lids.*"
                        "guid[[:space:]]+0x([0-9a-f]{16}).*:", REG_EXTENDED);

                regex_t route_re;
                regcomp(&route_re, "^"
                        "0x([0-9a-f]+)[[:space:]]+"            // Dest lid
                        "([[:digit:]]+)[[:space:]]+"           // Port id
                        ":[[:space:]]+[(]"                     // Separator
                        "(Channel Adapter|Switch)[[:space:]]+" // Type
                        "portguid 0x([0-9a-f]{16}):"           // Dest guid
                        ,
                        REG_EXTENDED);

                int read;

                route_source_t *route = NULL;
                while ((read = getline(&line, &size, route_file)) > 0) {
                    regmatch_t pmatch[5];
                    char *matches[5];
                    int port;
                    char dest_guid[MAX_STR];

                    if (!regexec(&header_re, line, (size_t)2, pmatch, 0)) {
                        char guid[MAX_STR];
                        get_match(line, 2, pmatch, matches);
                        sprintf(guid, "%.4s:%.4s:%.4s:%.4s",
                                matches[1], matches[1]+4, matches[1]+8, matches[1]+12);
                        for (int m = 0; m < 2; m++) {
                            free(matches[m]);
                        }

                        HASH_FIND_STR(routes, guid, route);
                        if (!route) {
                            route = (route_source_t *) malloc(sizeof(route_source_t));
                            sprintf(route->physical_id, "%s", guid);
                            route->dest = NULL;
                            HASH_ADD_STR(routes, physical_id, route);
                        }
                    }
                    else if (!regexec(&route_re, line, (size_t)5, pmatch, 0)) {
                        if (!route) {
                            fprintf(stderr, "Malformed route file %s\n", filename);
                            break;
                        }
                        route_dest_t *route_dest;
                        get_match(line, 5, pmatch, matches);
                        port = atoi(matches[2]);
                        sprintf(dest_guid, "%.4s:%.4s:%.4s:%.4s",
                                matches[4], matches[4]+4, matches[4]+8, matches[4]+12);
                        for (int m = 0; m < 5; m++) {
                            free(matches[m]);
                        }

                        route_dest = (route_dest_t *) malloc(sizeof(route_dest_t));
                        sprintf(route_dest->physical_id, "%s", dest_guid);
                        route_dest->port = port;
                        HASH_ADD_STR(route->dest, physical_id, route_dest);
                    }
                }
                fclose(route_file);
                regfree(&header_re);
                regfree(&route_re);
            }
        }
        free(line);
        regfree(&route_filename_regexp);
    }

    free(route_path);
    closedir(dir);
    *proutes = routes;

    return NETLOC_SUCCESS;
}