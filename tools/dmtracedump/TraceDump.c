/* //device/tools/dmtracedump/TraceDump.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/*
 * Process dmtrace output.
 *
 * This is the wrong way to go about it -- C is a clumsy language for
 * shuffling data around.  It'll do for a first pass.
 */
#define NOT_VM
#include "Profile.h"        // from VM header

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

/* Version number in the key file.  Version 1 uses one byte for the thread id.
 * Version 2 uses two bytes for the thread ids.
 */
int versionNumber;

/* arbitrarily limit indentation */
#define MAX_STACK_DEPTH 10000

/* thread list in key file is not reliable, so just max out */
#define MAX_THREADS     32768

/* Size of temporary buffers for escaping html strings */
#define HTML_BUFSIZE 10240

/* Size of methodId->method cache */
#define METHOD_CACHE_SIZE 2048
#define METHOD_CACHE_SIZE_MASK (METHOD_CACHE_SIZE - 1)

/* Some filter constants */
#define FILTER_TAG '*'
#define FILTER_FLAG_THREAD '+'
#define FILTER_TYPE_CLASS 0
#define FILTER_TYPE_METHOD 1

#define DEFAULT_ACTIVE_THREADS 8

char *htmlHeader =
"<html>\n<head>\n<script type=\"text/javascript\" src=\"%ssortable.js\"></script>\n"
"<script langugage=\"javascript\">\n"
"function toggle(item) {\n"
"    obj=document.getElementById(item);\n"
"    visible=(obj.style.display!=\"none\" && obj.style.display!=\"\");\n"
"    key=document.getElementById(\"x\" + item);\n"
"    if (visible) {\n"
"        obj.style.display=\"none\";\n"
"        key.innerHTML=\"+\";\n"
"    } else {\n"
"        obj.style.display=\"block\";\n"
"        key.innerHTML=\"-\";\n"
"    }\n"
"}\n"
"function onMouseOver(obj) {\n"
"    obj.style.background=\"lightblue\";\n"
"}\n"
"function onMouseOut(obj) {\n"
"    obj.style.background=\"white\";\n"
"}\n"
"</script>\n"
"<style type=\"text/css\">\n"
"div { font-family: courier; font-size: 13 }\n"
"div.parent { margin-left: 15; display: none }\n"
"div.leaf { margin-left: 10 }\n"
"div.header { margin-left: 10 }\n"
"div.link { margin-left: 10; cursor: move }\n"
"span.parent { padding-right: 10; }\n"
"span.leaf { padding-right: 10; }\n"
"a img { border: 0;}\n"
"table.sortable th { border-width: 0px 1px 1px 1px; background-color: #ccc;}\n"
"a { text-decoration: none; }\n"
"a:hover { text-decoration: underline; }\n"
"table.sortable th, table.sortable td { text-align: left;}"
"table.sortable tr.odd td { background-color: #ddd; }\n"
"table.sortable tr.even td { background-color: #fff; }\n"
"</style>\n"
"</head><body>\n\n";

char *htmlFooter = "\n</body>\n</html>\n";
char *profileSeparator =
    "======================================================================";

const char* tableHeader =
    "<table class='sortable' id='%s'><tr>\n"
    "<th>Method</th>\n"
    "<th>Run 1 (us)</th>\n"
    "<th>Run 2 (us)</th>\n"
    "<th>Diff (us)</th>\n"
    "<th>Diff (%%)</th>\n"
    "<th>1: # calls</th>\n"
    "<th>2: # calls</th>\n"
    "</tr>\n";

const char* tableHeaderMissing =
    "<table class='sortable' id='%s'>\n"
    "<th>Method</th>\n"
    "<th>Exclusive</th>\n"
    "<th>Inclusive</th>\n"
    "<th># calls</th>\n";

#define GRAPH_LABEL_VISITED 0x0001
#define GRAPH_NODE_VISITED  0x0002

/*
 * Values from the header of the data file.
 */
typedef struct DataHeader {
    unsigned int magic;
    short version;
    short offsetToData;
    long long startWhen;
} DataHeader;

/*
 * Entry from the thread list.
 */
typedef struct ThreadEntry {
    int         threadId;
    const char* threadName;
    uint64_t    elapsedTime;
} ThreadEntry;

struct MethodEntry;
typedef struct TimedMethod {
    struct TimedMethod *next;
    uint64_t elapsedInclusive;
    int numCalls;
    struct MethodEntry *method;
} TimedMethod;

typedef struct ClassEntry {
    const char *className;
    uint64_t elapsedExclusive;
    int numMethods;
    struct MethodEntry **methods;       /* list of methods in this class */
    int numCalls[2];                    /* 0=normal, 1=recursive */
} ClassEntry;

typedef struct UniqueMethodEntry {
    uint64_t elapsedExclusive;
    int numMethods;
    struct MethodEntry **methods;       /* list of methods with same name */
    int numCalls[2];                    /* 0=normal, 1=recursive */
} UniqueMethodEntry;

/*
 * Entry from the method list.
 */
typedef struct MethodEntry {
    unsigned int methodId;
    const char* className;
    const char* methodName;
    const char* signature;
    const char* fileName;
    int lineNum;
    uint64_t elapsedExclusive;
    uint64_t elapsedInclusive;
    uint64_t topExclusive;              /* non-recursive exclusive time */
    uint64_t recursiveInclusive;
    struct TimedMethod *parents[2];     /* 0=normal, 1=recursive */
    struct TimedMethod *children[2];    /* 0=normal, 1=recursive */
    int numCalls[2];                    /* 0=normal, 1=recursive */
    int index;                  /* used after sorting to number methods */
    int recursiveEntries;       /* number of entries on the stack */
    int graphState;             /* used when graphing to see if this method has been visited before */
} MethodEntry;

/*
 * The parsed contents of the key file.
 */
typedef struct DataKeys {
    char*        fileData;      /* contents of the entire file */
    long         fileLen;
    int          numThreads;
    ThreadEntry* threads;
    int          numMethods;
    MethodEntry* methods;       /* 2 extra methods: "toplevel" and "unknown" */
    int*         methodCache;   /* methodId->methodIndex mapping */
    // TODO change to map methodId->method itself
} DataKeys;

#define TOPLEVEL_INDEX 0
#define UNKNOWN_INDEX 1

typedef struct StackEntry {
    MethodEntry *method;
    uint64_t    entryTime;
} StackEntry;

typedef struct CallStack {
    int           top;
    StackEntry    calls[MAX_STACK_DEPTH];
    uint64_t      lastEventTime;
    uint64_t      threadStartTime;
    uint64_t*     remTimes;
    // Note: remTimes keeps a sum of 'un-allocated' time for each thread, in case
    // we need to allocate it to one (or many) filter later. This would happen when
    // we see a method exit that maches a filter, but whose entry we hadn't seen.
    // TODO: consider moving remTimes into FilterTimes and change logic appropriately
} CallStack;

typedef struct DiffEntry {
    MethodEntry* method1;
    MethodEntry* method2;
    int64_t differenceExclusive;
    int64_t differenceInclusive;
    double differenceExclusivePercentage;
    double differenceInclusivePercentage;
} DiffEntry;

// Global options
typedef struct Options {
    const char* traceFileName;
    const char* diffFileName;
    const char* graphFileName;
    const char* filterFileName;
    int keepDotFile;
    int dump;
    int outputHtml;
    const char* sortableUrl;
    int threshold;
} Options;

typedef struct TraceData {
    int numClasses;
    ClassEntry *classes;
    CallStack *stacks[MAX_THREADS];
    int depth[MAX_THREADS];
    int numUniqueMethods;
    UniqueMethodEntry *uniqueMethods;
} TraceData;

typedef struct FilterKey {
    int       type[2];    /* 0=class, 1=method; 2 needed for start and end keys */
    uint32_t  flags;      /* 1st bit = include cross-thread time */
    char*     keys[2];    /* 2 needed for start and end keys */
} FilterKey;

typedef struct FilterTimes {
    uint64_t   totalWaitTime;
    uint64_t*  threadWaitTimes;
    uint64_t*  threadExecutionTimesWhileWaiting;
    uint64_t*  threadExecutionTimes;
} FilterTimes;

typedef struct Filter {
    char*       filterName;
    FilterKey*  filterKeys;
    int         numKeys;
    int         activeCount;
    int*        activeThreads;
    int*        activationKeys;
    FilterTimes times;
} Filter;

int numFilters = 0; // global

static Options gOptions;

/* Escapes characters in the source string that are html special entities.
 * The escaped string is written to "dest" which must be large enough to
 * hold the result.  A pointer to "dest" is returned.  The characters and
 * their corresponding escape sequences are:
 *  '<'  &lt;
 *  '>'  &gt;
 *  '&'  &amp;
 */
char *htmlEscape(const char *src, char *dest, int len)
{
    char *destStart = dest;

    if (src == NULL)
        return NULL;

    int nbytes = 0;
    while (*src) {
        if (*src == '<') {
            nbytes += 4;
            if (nbytes >= len)
                break;
            *dest++ = '&';
            *dest++ = 'l';
            *dest++ = 't';
            *dest++ = ';';
        } else if (*src == '>') {
            nbytes += 4;
            if (nbytes >= len)
                break;
            *dest++ = '&';
            *dest++ = 'g';
            *dest++ = 't';
            *dest++ = ';';
        } else if (*src == '&') {
            nbytes += 5;
            if (nbytes >= len)
                break;
            *dest++ = '&';
            *dest++ = 'a';
            *dest++ = 'm';
            *dest++ = 'p';
            *dest++ = ';';
        } else {
            nbytes += 1;
            if (nbytes >= len)
                break;
            *dest++ = *src;
        }
        src += 1;
    }
    if (nbytes >= len) {
        fprintf(stderr, "htmlEscape(): buffer overflow\n");
        exit(1);
    }
    *dest = 0;

    return destStart;
}

/* Initializes a MethodEntry
 */
void initMethodEntry(MethodEntry *method, unsigned int methodId,
                     const char *className, const char *methodName,
                     const char *signature, const char* fileName,
                     const char* lineNumStr)
{
    method->methodId = methodId;
    method->className = className;
    method->methodName = methodName;
    method->signature = signature;
    method->fileName = fileName;
    method->lineNum = (lineNumStr != NULL) ? atoi(lineNumStr) : -1;
    method->elapsedExclusive = 0;
    method->elapsedInclusive = 0;
    method->topExclusive = 0;
    method->recursiveInclusive = 0;
    method->parents[0] = NULL;
    method->parents[1] = NULL;
    method->children[0] = NULL;
    method->children[1] = NULL;
    method->numCalls[0] = 0;
    method->numCalls[1] = 0;
    method->index = 0;
    method->recursiveEntries = 0;
}

/*
 * This comparison function is called from qsort() to sort
 * methods into decreasing order of exclusive elapsed time.
 */
int compareElapsedExclusive(const void *a, const void *b) {
    uint64_t elapsed1, elapsed2;
    int result;

    const MethodEntry *methodA = *(const MethodEntry**)a;
    const MethodEntry *methodB = *(const MethodEntry**)b;
    elapsed1 = methodA->elapsedExclusive;
    elapsed2 = methodB->elapsedExclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two methods are equal, then sort them
     * into alphabetical order.
     */
    result = strcmp(methodA->className, methodB->className);
    if (result == 0) {
        if (methodA->methodName == NULL || methodB->methodName == NULL) {
            unsigned int idA = methodA->methodId;
            unsigned int idB = methodB->methodId;
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
            return 0;
        }
        result = strcmp(methodA->methodName, methodB->methodName);
        if (result == 0)
            result = strcmp(methodA->signature, methodB->signature);
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * methods into decreasing order of inclusive elapsed time.
 */
int compareElapsedInclusive(const void *a, const void *b) {
    const MethodEntry *methodA, *methodB;
    uint64_t elapsed1, elapsed2;
    int result;

    methodA = *(MethodEntry const **)a;
    methodB = *(MethodEntry const **)b;
    elapsed1 = methodA->elapsedInclusive;
    elapsed2 = methodB->elapsedInclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two methods are equal, then sort them
     * into alphabetical order.
     */
    result = strcmp(methodA->className, methodB->className);
    if (result == 0) {
        if (methodA->methodName == NULL || methodB->methodName == NULL) {
            unsigned int idA = methodA->methodId;
            unsigned int idB = methodB->methodId;
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
            return 0;
        }
        result = strcmp(methodA->methodName, methodB->methodName);
        if (result == 0)
            result = strcmp(methodA->signature, methodB->signature);
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * threads into decreasing order of elapsed time.
 */
int compareElapsed(const void *a, const void *b) {
    const ThreadEntry *threadA, *threadB;
    uint64_t elapsed1, elapsed2;
    int result = 0;

    threadA = (ThreadEntry const *)a;
    threadB = (ThreadEntry const *)b;
    elapsed1 = threadA->elapsedTime;
    elapsed2 = threadB->elapsedTime;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two threads are equal, then sort them
     * by thread id.
     */
    int idA = threadA->threadId;
    int idB = threadB->threadId;
    if (idA < idB)
        result = -1;
    if (idA > idB)
        result = 1;

    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * TimedMethods into decreasing order of inclusive elapsed time.
 */
int compareTimedMethod(const void *a, const void *b) {
    const TimedMethod *timedA, *timedB;
    uint64_t elapsed1, elapsed2;
    int result;

    timedA = (TimedMethod const *)a;
    timedB = (TimedMethod const *)b;
    elapsed1 = timedA->elapsedInclusive;
    elapsed2 = timedB->elapsedInclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two methods are equal, then sort them
     * into alphabetical order.
     */
    MethodEntry *methodA = timedA->method;
    MethodEntry *methodB = timedB->method;
    result = strcmp(methodA->className, methodB->className);
    if (result == 0) {
        if (methodA->methodName == NULL || methodB->methodName == NULL) {
            unsigned int idA = methodA->methodId;
            unsigned int idB = methodB->methodId;
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
            return 0;
        }
        result = strcmp(methodA->methodName, methodB->methodName);
        if (result == 0)
            result = strcmp(methodA->signature, methodB->signature);
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * MethodEntry pointers into alphabetical order of class names.
 */
int compareClassNames(const void *a, const void *b) {
    int result;

    const MethodEntry *methodA = *(const MethodEntry**)a;
    const MethodEntry *methodB = *(const MethodEntry**)b;
    result = strcmp(methodA->className, methodB->className);
    if (result == 0) {
        unsigned int idA = methodA->methodId;
        unsigned int idB = methodB->methodId;
        if (idA < idB)
            return -1;
        if (idA > idB)
            return 1;
        return 0;
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * classes into decreasing order of exclusive elapsed time.
 */
int compareClassExclusive(const void *a, const void *b) {
    uint64_t elapsed1, elapsed2;
    int result;

    const ClassEntry *classA = *(const ClassEntry**)a;
    const ClassEntry *classB = *(const ClassEntry**)b;
    elapsed1 = classA->elapsedExclusive;
    elapsed2 = classB->elapsedExclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two classs are equal, then sort them
     * into alphabetical order.
     */
    result = strcmp(classA->className, classB->className);
    if (result == 0) {
        /* Break ties with the first method id.  This is probably not
         * needed.
         */
        unsigned int idA = classA->methods[0]->methodId;
        unsigned int idB = classB->methods[0]->methodId;
        if (idA < idB)
            return -1;
        if (idA > idB)
            return 1;
        return 0;
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * MethodEntry pointers into alphabetical order by method name,
 * then by class name.
 */
int compareMethodNames(const void *a, const void *b) {
    int result;

    const MethodEntry *methodA = *(const MethodEntry**)a;
    const MethodEntry *methodB = *(const MethodEntry**)b;
    if (methodA->methodName == NULL || methodB->methodName == NULL) {
        return compareClassNames(a, b);
    }
    result = strcmp(methodA->methodName, methodB->methodName);
    if (result == 0) {
        result = strcmp(methodA->className, methodB->className);
        if (result == 0) {
            unsigned int idA = methodA->methodId;
            unsigned int idB = methodB->methodId;
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
            return 0;
        }
    }
    return result;
}

/*
 * This comparison function is called from qsort() to sort
 * unique methods into decreasing order of exclusive elapsed time.
 */
int compareUniqueExclusive(const void *a, const void *b) {
    uint64_t elapsed1, elapsed2;
    int result;

    const UniqueMethodEntry *uniqueA = *(const UniqueMethodEntry**)a;
    const UniqueMethodEntry *uniqueB = *(const UniqueMethodEntry**)b;
    elapsed1 = uniqueA->elapsedExclusive;
    elapsed2 = uniqueB->elapsedExclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two methods are equal, then sort them
     * into alphabetical order.
     */
    result = strcmp(uniqueA->methods[0]->className,
                    uniqueB->methods[0]->className);
    if (result == 0) {
        unsigned int idA = uniqueA->methods[0]->methodId;
        unsigned int idB = uniqueB->methods[0]->methodId;
        if (idA < idB)
            return -1;
        if (idA > idB)
            return 1;
        return 0;
    }
    return result;
}

/*
 * Free a DataKeys struct.
 */
void freeDataKeys(DataKeys* pKeys)
{
    if (pKeys == NULL)
        return;

    free(pKeys->fileData);
    free(pKeys->threads);
    free(pKeys->methods);
    free(pKeys->methodCache);
    free(pKeys);
}

/*
 * Find the offset to the next occurrence of the specified character.
 *
 * "data" should point somewhere within the current line.  "len" is the
 * number of bytes left in the buffer.
 *
 * Returns -1 if we hit the end of the buffer.
 */
int findNextChar(const char* data, int len, char lookFor)
{
    const char* start = data;

    while (len > 0) {
        if (*data == lookFor)
            return data - start;

        data++;
        len--;
    }

    return -1;
}

int countLinesToChar(const char* data, int len, const char toFind)
{
    int count = 0;
    int next;

    while (*data != toFind) {
        next = findNextChar(data, len, '\n');
        if (next < 0)
	    return count;
        count++;
        data += next+1;
        len -= next+1;
    }

    return count;
}

/*
 * Count the number of lines until the next token.
 *
 * Returns 0 if none found before EOF.
 */
int countLinesToToken(const char* data, int len)
{
    return countLinesToChar(data, len, TOKEN_CHAR);
}

/*
 * Make sure we're at the start of the right section.
 *
 * Returns the length of the token line, or -1 if something is wrong.
 */
int checkToken(const char* data, int len, const char* cmpStr)
{
    int cmpLen = strlen(cmpStr);
    int next;

    if (*data != TOKEN_CHAR) {
        fprintf(stderr,
            "ERROR: not at start of %s (found '%.10s')\n", cmpStr, data);
        return -1;
    }

    next = findNextChar(data, len, '\n');
    if (next < cmpLen+1)
        return -1;

    if (strncmp(data+1, cmpStr, cmpLen) != 0) {
        fprintf(stderr, "ERROR: '%s' not found (got '%.7s')\n", cmpStr, data+1);
        return -1;
    }

    return next+1;
}

/*
 * Parse the "*version" section.
 */
long parseVersion(DataKeys* pKeys, long offset, int verbose)
{
    char* data;
    char* dataEnd;
    int i, count, next;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "version");
    if (next <= 0)
        return -1;

    data += next;

    /*
     * Count the number of items in the "version" section.
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading version (found %d)\n", count);
        return -1;
    }

    /* find the end of the line */
    next = findNextChar(data, dataEnd - data, '\n');
    if (next < 0)
        return -1;

    data[next] = '\0';
    versionNumber = strtoul(data, NULL, 0);
    if (verbose)
        printf("VERSION: %d\n", versionNumber);

    data += next+1;

    /* skip over the rest of the stuff, which is "name=value" lines */
    for (i = 1; i < count; i++) {
        next = findNextChar(data, dataEnd - data, '\n');
        if (next < 0)
            return -1;
        //data[next] = '\0';
        //printf("IGNORING: '%s'\n", data);
        data += next+1;
    }

    return data - pKeys->fileData;
}

/*
 * Parse the "*threads" section.
 */
long parseThreads(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int i, next, tab, count;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "threads");

    data += next;

    /*
     * Count the number of thread entries (one per line).
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading threads (found %d)\n", count);
        return -1;
    }

    //printf("+++ found %d threads\n", count);
    pKeys->threads = (ThreadEntry*) malloc(sizeof(ThreadEntry) * count);
    if (pKeys->threads == NULL)
        return -1;

    /*
     * Extract all entries.
     */
    for (i = 0; i < count; i++) {
        next = findNextChar(data, dataEnd - data, '\n');
        assert(next > 0);
        data[next] = '\0';

        tab = findNextChar(data, next, '\t');
        data[tab] = '\0';

        pKeys->threads[i].threadId = atoi(data);
        pKeys->threads[i].threadName = data + tab +1;

        data += next+1;
    }

    pKeys->numThreads = count;
    return data - pKeys->fileData;
}

/*
 * Parse the "*methods" section.
 */
long parseMethods(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int i, next, count;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "methods");
    if (next < 0)
        return -1;

    data += next;

    /*
     * Count the number of method entries (one per line).
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading methods (found %d)\n", count);
        return -1;
    }

    /* Reserve an extra method at location 0 for the "toplevel" method,
     * and another extra method for all other "unknown" methods.
     */
    count += 2;
    pKeys->methods = (MethodEntry*) malloc(sizeof(MethodEntry) * count);
    if (pKeys->methods == NULL)
        return -1;
    initMethodEntry(&pKeys->methods[TOPLEVEL_INDEX], 0, "(toplevel)",
        NULL, NULL, NULL, NULL);
    initMethodEntry(&pKeys->methods[UNKNOWN_INDEX], 0, "(unknown)",
        NULL, NULL, NULL, NULL);

    /*
     * Extract all entries, starting with index 2.
     */
    for (i = UNKNOWN_INDEX + 1; i < count; i++) {
        int tab1, tab2, tab3, tab4, tab5;
        unsigned int id;
        char* endptr;

        next = findNextChar(data, dataEnd - data, '\n');
        assert(next > 0);
        data[next] = '\0';

        tab1 = findNextChar(data, next, '\t');
        tab2 = findNextChar(data+(tab1+1), next-(tab1+1), '\t');
        tab3 = findNextChar(data+(tab1+tab2+2), next-(tab1+tab2+2), '\t');
        tab4 = findNextChar(data+(tab1+tab2+tab3+3),
                            next-(tab1+tab2+tab3+3), '\t');
        tab5 = findNextChar(data+(tab1+tab2+tab3+tab4+4),
                            next-(tab1+tab2+tab3+tab4+4), '\t');
        if (tab1 < 0) {
            fprintf(stderr, "ERROR: missing field on method line: '%s'\n",
                    data);
            return -1;
        }
        assert(data[tab1] == '\t');
        data[tab1] = '\0';

        id = strtoul(data, &endptr, 0);
        if (*endptr != '\0') {
            fprintf(stderr, "ERROR: bad method ID '%s'\n", data);
            return -1;
        }

        // Allow files that specify just a function name, instead of requiring
        // "class \t method \t signature"
        if (tab2 > 0 && tab3 > 0) {
            tab2 += tab1+1;
            tab3 += tab2+1;
            assert(data[tab2] == '\t');
            assert(data[tab3] == '\t');
            data[tab2] = data[tab3] = '\0';

            // This is starting to get awkward.  Allow filename and line #.
            if (tab4 > 0 && tab5 > 0) {
                tab4 += tab3+1;
                tab5 += tab4+1;

                assert(data[tab4] == '\t');
                assert(data[tab5] == '\t');
                data[tab4] = data[tab5] = '\0';

                initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                        data + tab2 +1, data + tab3 +1, data + tab4 +1,
                        data + tab5 +1);
            } else {
                initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                        data + tab2 +1, data + tab3 +1, NULL, NULL);
            }
        } else {
            initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                NULL, NULL, NULL, NULL);
        }

        data += next+1;
    }

    pKeys->numMethods = count;
    return data - pKeys->fileData;
}

/*
 * Parse the "*end" section.
 */
long parseEnd(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int next;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "end");
    if (next < 0)
        return -1;

    data += next;

    return data - pKeys->fileData;
}

/*
 * Sort the thread list entries.
 */
static int compareThreads(const void* thread1, const void* thread2)
{
    return ((const ThreadEntry*) thread1)->threadId -
            ((const ThreadEntry*) thread2)->threadId;
}

void sortThreadList(DataKeys* pKeys)
{
    qsort(pKeys->threads, pKeys->numThreads, sizeof(pKeys->threads[0]),
        compareThreads);
}

/*
 * Sort the method list entries.
 */
static int compareMethods(const void* meth1, const void* meth2)
{
    unsigned int id1, id2;

    id1 = ((const MethodEntry*) meth1)->methodId;
    id2 = ((const MethodEntry*) meth2)->methodId;
    if (id1 < id2)
        return -1;
    if (id1 > id2)
        return 1;
    return 0;
}

void sortMethodList(DataKeys* pKeys)
{
    qsort(pKeys->methods, pKeys->numMethods, sizeof(MethodEntry),
        compareMethods);
}

/*
 * Parse the key section, and return a copy of the parsed contents.
 */
DataKeys* parseKeys(FILE *fp, int verbose)
{
    DataKeys* pKeys = NULL;
    long offset;
    int i;

    pKeys = (DataKeys*) calloc(1, sizeof(DataKeys));
    if (pKeys == NULL)
        goto fail;

    /*
     * We load the entire file into memory.  We do this, rather than memory-
     * mapping it, because we want to change some whitespace to NULs.
     */
    if (fseek(fp, 0L, SEEK_END) != 0) {
        perror("fseek");
        goto fail;
    }
    pKeys->fileLen = ftell(fp);
    if (pKeys->fileLen == 0) {
        fprintf(stderr, "Key file is empty.\n");
        goto fail;
    }
    rewind(fp);

    pKeys->fileData = (char*) malloc(pKeys->fileLen);
    if (pKeys->fileData == NULL) {
        fprintf(stderr, "ERROR: unable to alloc %ld bytes\n", pKeys->fileLen);
        goto fail;
    }

    if (fread(pKeys->fileData, 1, pKeys->fileLen, fp) != (size_t) pKeys->fileLen)
    {
        fprintf(stderr, "ERROR: unable to read %ld bytes from trace file\n",
            pKeys->fileLen);
        goto fail;
    }

    offset = 0;

    offset = parseVersion(pKeys, offset, verbose);
    offset = parseThreads(pKeys, offset);
    offset = parseMethods(pKeys, offset);
    offset = parseEnd(pKeys, offset);
    if (offset < 0)
        goto fail;

    /* Leave fp pointing to the beginning of the data section. */
    fseek(fp, offset, SEEK_SET);

    sortThreadList(pKeys);
    sortMethodList(pKeys);

    /*
     * Dump list of threads.
     */
    if (verbose) {
        printf("Threads (%d):\n", pKeys->numThreads);
        for (i = 0; i < pKeys->numThreads; i++) {
            printf("%2d %s\n",
                   pKeys->threads[i].threadId, pKeys->threads[i].threadName);
        }
    }

#if 0
    /*
     * Dump list of methods.
     */
    if (verbose) {
        printf("Methods (%d):\n", pKeys->numMethods);
        for (i = 0; i < pKeys->numMethods; i++) {
            printf("0x%08x %s : %s : %s\n",
                   pKeys->methods[i].methodId >> 2, pKeys->methods[i].className,
                   pKeys->methods[i].methodName, pKeys->methods[i].signature);
        }
    }
#endif

    return pKeys;

fail:
    freeDataKeys(pKeys);
    return NULL;
}


/*
 * Read values from the binary data file.
 */

/* Make the return value "unsigned int" instead of "unsigned short" so that
 * we can detect EOF.
 */
unsigned int read2LE(FILE* fp)
{
    unsigned int val;

    val = getc(fp);
    val |= getc(fp) << 8;
    return val;
}
unsigned int read4LE(FILE* fp)
{
    unsigned int val;

    val = getc(fp);
    val |= getc(fp) << 8;
    val |= getc(fp) << 16;
    val |= getc(fp) << 24;
    return val;
}
unsigned long long read8LE(FILE* fp)
{
    unsigned long long val;

    val = getc(fp);
    val |= (unsigned long long) getc(fp) << 8;
    val |= (unsigned long long) getc(fp) << 16;
    val |= (unsigned long long) getc(fp) << 24;
    val |= (unsigned long long) getc(fp) << 32;
    val |= (unsigned long long) getc(fp) << 40;
    val |= (unsigned long long) getc(fp) << 48;
    val |= (unsigned long long) getc(fp) << 56;
    return val;
}

/*
 * Parse the header of the data section.
 *
 * Returns with the file positioned at the start of the record data.
 */
int parseDataHeader(FILE *fp, DataHeader* pHeader)
{
    pHeader->magic = read4LE(fp);
    pHeader->version = read2LE(fp);
    pHeader->offsetToData = read2LE(fp);
    pHeader->startWhen = read8LE(fp);

    if (fseek(fp, pHeader->offsetToData - 16, SEEK_CUR) != 0) {
        return -1;
    }

    return 0;
}

/*
 * Look up a method by its method ID (using binary search).
 *
 * Returns NULL if no matching method was found.
 */
MethodEntry* lookupMethod(DataKeys* pKeys, unsigned int methodId)
{
    int hi, lo, mid;
    unsigned int id;
    int hashedId;

    /* Create cache if it doesn't already exist */
    if (pKeys->methodCache == NULL) {
        pKeys->methodCache = (int*) calloc(METHOD_CACHE_SIZE, sizeof(int));
    }

    // ids are multiples of 4, so shift
    hashedId = (methodId >> 2) & METHOD_CACHE_SIZE_MASK;
    if (pKeys->methodCache[hashedId]) /* cache hit */
        if (pKeys->methods[pKeys->methodCache[hashedId]].methodId == methodId)
	    return &pKeys->methods[pKeys->methodCache[hashedId]];

    lo = 0;
    hi = pKeys->numMethods - 1;

    while (hi >= lo) {
        mid = (hi + lo) / 2;

        id = pKeys->methods[mid].methodId;
        if (id == methodId) {         /* match, put in cache */
	    hashedId = (methodId >> 2) & METHOD_CACHE_SIZE_MASK;
	    pKeys->methodCache[hashedId] = mid;
	    return &pKeys->methods[mid];
	} else if (id < methodId)       /* too low */
            lo = mid + 1;
        else                          /* too high */
            hi = mid - 1;
    }

    return NULL;
}

/*
 * Reads the next data record, and assigns the data values to threadId,
 * methodVal and elapsedTime.  On end-of-file, the threadId, methodVal,
 * and elapsedTime are unchanged.  Returns 1 on end-of-file, otherwise
 * returns 0.
 */
int readDataRecord(FILE *dataFp, int *threadId, unsigned int *methodVal,
                   uint64_t *elapsedTime)
{
    int id;

    /*
     * TODO:
     * This SHOULD NOT be keyed off of the global version number!  Use
     * a name=value setting in the version area instead!
     */
    if (versionNumber == 1) {
        id = getc(dataFp);
    } else {
        id = read2LE(dataFp);
    }
    if (id == EOF)
        return 1;
    *threadId = id;

    *methodVal = read4LE(dataFp);
    *elapsedTime = read4LE(dataFp);
    if (feof(dataFp)) {
        fprintf(stderr, "WARNING: hit EOF mid-record\n");
        return 1;
    }
    return 0;
}

/*
 * Read the key file and use it to produce formatted output from the
 * data file.
 */
void dumpTrace()
{
    static const char* actionStr[] = { "ent", "xit", "unr", "???" };
    MethodEntry bogusMethod = { 0, "???", "???", "???", "???", -1, 0, 0, 0, 0,
                                {NULL, NULL}, {NULL, NULL}, {0, 0}, 0, 0, -1 };
    char bogusBuf[80];
    char spaces[MAX_STACK_DEPTH+1];
    FILE* dataFp = NULL;
    DataHeader dataHeader;
    DataKeys* pKeys = NULL;
    int i;
    TraceData traceData;

    //printf("Dumping '%s' '%s'\n", dataFileName, keyFileName);

    memset(spaces, '.', MAX_STACK_DEPTH);
    spaces[MAX_STACK_DEPTH] = '\0';

    for (i = 0; i < MAX_THREADS; i++)
        traceData.depth[i] = 2;       // adjust for return from start function

    dataFp = fopen(gOptions.traceFileName, "r");
    if (dataFp == NULL)
        goto bail;

    if ((pKeys = parseKeys(dataFp, 1)) == NULL)
        goto bail;

    if (parseDataHeader(dataFp, &dataHeader) < 0)
        goto bail;

    printf("Trace (threadID action usecs class.method signature):\n");

    while (1) {
        MethodEntry* method;
        int threadId;
        unsigned int methodVal;
        uint64_t elapsedTime;
        int action, printDepth;
        unsigned int methodId, lastEnter = 0;
        int mismatch = 0;
        char depthNote;

        /*
         * Extract values from file.
         */
        if (readDataRecord(dataFp, &threadId, &methodVal, &elapsedTime))
            break;

        action = METHOD_ACTION(methodVal);
        methodId = METHOD_ID(methodVal);

        /*
         * Generate a line of output.
         */
        if (action == METHOD_TRACE_ENTER) {
            traceData.depth[threadId]++;
            lastEnter = methodId;
        } else {
            /* quick test for mismatched adjacent enter/exit */
            if (lastEnter != 0 && lastEnter != methodId)
                mismatch = 1;
        }

        printDepth = traceData.depth[threadId];
        depthNote = ' ';
        if (printDepth < 0) {
            printDepth = 0;
            depthNote = '-';
        } else if (printDepth > MAX_STACK_DEPTH) {
            printDepth = MAX_STACK_DEPTH;
            depthNote = '+';
        }

        method = lookupMethod(pKeys, methodId);
        if (method == NULL) {
            method = &bogusMethod;
            sprintf(bogusBuf, "methodId: 0x%x", methodId);
            method->signature = bogusBuf;
        }

	if (method->methodName) {
	    printf("%2d %s%c %8lld%c%s%s.%s %s\n", threadId,
		   actionStr[action], mismatch ? '!' : ' ',
		   elapsedTime, depthNote,
		   spaces + (MAX_STACK_DEPTH - printDepth),
		   method->className, method->methodName, method->signature);
	} else {
	    printf("%2d %s%c %8lld%c%s%s\n", threadId,
		   actionStr[action], mismatch ? '!' : ' ',
		   elapsedTime, depthNote,
		   spaces + (MAX_STACK_DEPTH - printDepth),
		   method->className);
	}

        if (action != METHOD_TRACE_ENTER) {
            traceData.depth[threadId]--;  /* METHOD_TRACE_EXIT or METHOD_TRACE_UNROLL */
            lastEnter = 0;
        }

        mismatch = 0;
    }

bail:
    if (dataFp != NULL)
        fclose(dataFp);
    if (pKeys != NULL)
        freeDataKeys(pKeys);
}

/* This routine adds the given time to the parent and child methods.
 * This is called when the child routine exits, after the child has
 * been popped from the stack.  The elapsedTime parameter is the
 * duration of the child routine, including time spent in called routines.
 */
void addInclusiveTime(MethodEntry *parent, MethodEntry *child,
                      uint64_t elapsedTime)
{
    TimedMethod *pTimed;

#if 0
    bool verbose = false;
    if (strcmp(child->className, debugClassName) == 0)
        verbose = true;
#endif

    int childIsRecursive = (child->recursiveEntries > 0);
    int parentIsRecursive = (parent->recursiveEntries > 1);

    if (child->recursiveEntries == 0) {
        child->elapsedInclusive += elapsedTime;
    } else if (child->recursiveEntries == 1) {
        child->recursiveInclusive += elapsedTime;
    }
    child->numCalls[childIsRecursive] += 1;

#if 0
    if (verbose) {
        fprintf(stderr,
                "%s %d elapsedTime: %lld eI: %lld, rI: %lld\n",
                child->className, child->recursiveEntries,
                elapsedTime, child->elapsedInclusive,
                child->recursiveInclusive);
    }
#endif

    /* Find the child method in the parent */
    TimedMethod *children = parent->children[parentIsRecursive];
    for (pTimed = children; pTimed; pTimed = pTimed->next) {
        if (pTimed->method == child) {
            pTimed->elapsedInclusive += elapsedTime;
            pTimed->numCalls += 1;
            break;
        }
    }
    if (pTimed == NULL) {
        /* Allocate a new TimedMethod */
        pTimed = (TimedMethod *) malloc(sizeof(TimedMethod));
        pTimed->elapsedInclusive = elapsedTime;
        pTimed->numCalls = 1;
        pTimed->method = child;

        /* Add it to the front of the list */
        pTimed->next = children;
        parent->children[parentIsRecursive] = pTimed;
    }

    /* Find the parent method in the child */
    TimedMethod *parents = child->parents[childIsRecursive];
    for (pTimed = parents; pTimed; pTimed = pTimed->next) {
        if (pTimed->method == parent) {
            pTimed->elapsedInclusive += elapsedTime;
            pTimed->numCalls += 1;
            break;
        }
    }
    if (pTimed == NULL) {
        /* Allocate a new TimedMethod */
        pTimed = (TimedMethod *) malloc(sizeof(TimedMethod));
        pTimed->elapsedInclusive = elapsedTime;
        pTimed->numCalls = 1;
        pTimed->method = parent;

        /* Add it to the front of the list */
        pTimed->next = parents;
        child->parents[childIsRecursive] = pTimed;
    }

#if 0
    if (verbose) {
        fprintf(stderr,
                "  %s %d eI: %lld\n",
                parent->className, parent->recursiveEntries,
                pTimed->elapsedInclusive);
    }
#endif
}

/* Sorts a linked list and returns a newly allocated array containing
 * the sorted entries.
 */
TimedMethod *sortTimedMethodList(TimedMethod *list, int *num)
{
    int ii;
    TimedMethod *pTimed, *sorted;

    /* Count the elements */
    int num_entries = 0;
    for (pTimed = list; pTimed; pTimed = pTimed->next)
        num_entries += 1;
    *num = num_entries;
    if (num_entries == 0)
        return NULL;

    /* Copy all the list elements to a new array and sort them */
    sorted = (TimedMethod *) malloc(sizeof(TimedMethod) * num_entries);
    for (ii = 0, pTimed = list; pTimed; pTimed = pTimed->next, ++ii)
        memcpy(&sorted[ii], pTimed, sizeof(TimedMethod));
    qsort(sorted, num_entries, sizeof(TimedMethod), compareTimedMethod);

    /* Fix up the "next" pointers so that they work. */
    for (ii = 0; ii < num_entries - 1; ++ii)
        sorted[ii].next = &sorted[ii + 1];
    sorted[num_entries - 1].next = NULL;

    return sorted;
}

/* Define flag values for printInclusiveMethod() */
static const int kIsRecursive = 1;

/* This prints the inclusive stats for all the parents or children of a
 * method, depending on the list that is passed in.
 */
void printInclusiveMethod(MethodEntry *method, TimedMethod *list, int numCalls,
                          int flags)
{
    int num;
    TimedMethod *pTimed;
    char buf[80];
    char *anchor_close;
    char *spaces = "      ";    /* 6 spaces */
    int num_spaces = strlen(spaces);
    char *space_ptr = &spaces[num_spaces];
    char *className, *methodName, *signature;
    char classBuf[HTML_BUFSIZE], methodBuf[HTML_BUFSIZE];
    char signatureBuf[HTML_BUFSIZE];

    anchor_close = "";
    if (gOptions.outputHtml)
        anchor_close = "</a>";

    TimedMethod *sorted = sortTimedMethodList(list,  &num);
    double methodTotal = method->elapsedInclusive;
    for (pTimed = sorted; pTimed; pTimed = pTimed->next) {
        MethodEntry *relative = pTimed->method;
        className = (char*)(relative->className);
        methodName = (char*)(relative->methodName);
        signature = (char*)(relative->signature);
        double per = 100.0 * pTimed->elapsedInclusive / methodTotal;
        sprintf(buf, "[%d]", relative->index);
        if (gOptions.outputHtml) {
            int len = strlen(buf);
            if (len > num_spaces)
                len = num_spaces;
            sprintf(buf, "<a href=\"#m%d\">[%d]",
                    relative->index, relative->index);
            space_ptr = &spaces[len];
            className = htmlEscape(className, classBuf, HTML_BUFSIZE);
            methodName = htmlEscape(methodName, methodBuf, HTML_BUFSIZE);
            signature = htmlEscape(signature, signatureBuf, HTML_BUFSIZE);
        }
        int nCalls = numCalls;
        if (nCalls == 0)
            nCalls = relative->numCalls[0] + relative->numCalls[1];
        if (relative->methodName) {
            if (flags & kIsRecursive) {
                // Don't display percentages for recursive functions
                printf("%6s %5s   %6s %s%6s%s %6d/%-6d %9llu %s.%s %s\n",
                       "", "", "",
                       space_ptr, buf, anchor_close,
                       pTimed->numCalls, nCalls,
                       pTimed->elapsedInclusive,
                       className, methodName, signature);
            } else {
                printf("%6s %5s   %5.1f%% %s%6s%s %6d/%-6d %9llu %s.%s %s\n",
                       "", "", per,
                       space_ptr, buf, anchor_close,
                       pTimed->numCalls, nCalls,
                       pTimed->elapsedInclusive,
                       className, methodName, signature);
            }
        } else {
            if (flags & kIsRecursive) {
                // Don't display percentages for recursive functions
                printf("%6s %5s   %6s %s%6s%s %6d/%-6d %9llu %s\n",
                       "", "", "",
                       space_ptr, buf, anchor_close,
                       pTimed->numCalls, nCalls,
                       pTimed->elapsedInclusive,
                       className);
            } else {
                printf("%6s %5s   %5.1f%% %s%6s%s %6d/%-6d %9llu %s\n",
                       "", "", per,
                       space_ptr, buf, anchor_close,
                       pTimed->numCalls, nCalls,
                       pTimed->elapsedInclusive,
                       className);
            }
        }
    }
}

void countRecursiveEntries(CallStack *pStack, int top, MethodEntry *method)
{
    int ii;

    method->recursiveEntries = 0;
    for (ii = 0; ii < top; ++ii) {
        if (pStack->calls[ii].method == method)
            method->recursiveEntries += 1;
    }
}

void stackDump(CallStack *pStack, int top)
{
    int ii;

    for (ii = 0; ii < top; ++ii) {
        MethodEntry *method = pStack->calls[ii].method;
        uint64_t entryTime = pStack->calls[ii].entryTime;
        if (method->methodName) {
            fprintf(stderr, "  %2d: %8llu %s.%s %s\n", ii, entryTime,
                   method->className, method->methodName, method->signature);
        } else {
            fprintf(stderr, "  %2d: %8llu %s\n", ii, entryTime, method->className);
        }
    }
}

void outputTableOfContents()
{
    printf("<a name=\"contents\"></a>\n");
    printf("<h2>Table of Contents</h2>\n");
    printf("<ul>\n");
    printf("  <li><a href=\"#exclusive\">Exclusive profile</a></li>\n");
    printf("  <li><a href=\"#inclusive\">Inclusive profile</a></li>\n");
    printf("  <li><a href=\"#thread\">Thread profile</a></li>\n");
    printf("  <li><a href=\"#class\">Class/method profile</a></li>\n");
    printf("  <li><a href=\"#method\">Method/class profile</a></li>\n");
    printf("</ul>\n\n");
}

void outputNavigationBar()
{
    printf("<a href=\"#contents\">[Top]</a>\n");
    printf("<a href=\"#exclusive\">[Exclusive]</a>\n");
    printf("<a href=\"#inclusive\">[Inclusive]</a>\n");
    printf("<a href=\"#thread\">[Thread]</a>\n");
    printf("<a href=\"#class\">[Class]</a>\n");
    printf("<a href=\"#method\">[Method]</a>\n");
    printf("<br><br>\n");
}

void printExclusiveProfile(MethodEntry **pMethods, int numMethods,
                           uint64_t sumThreadTime)
{
    int ii;
    MethodEntry* method;
    double total, sum, per, sum_per;
    char classBuf[HTML_BUFSIZE], methodBuf[HTML_BUFSIZE];
    char signatureBuf[HTML_BUFSIZE];
    char anchor_buf[80];
    char *anchor_close = "";

    total = sumThreadTime;
    anchor_buf[0] = 0;
    if (gOptions.outputHtml) {
        anchor_close = "</a>";
        printf("<a name=\"exclusive\"></a>\n");
        printf("<hr>\n");
        outputNavigationBar();
    } else {
        printf("\n%s\n", profileSeparator);
    }

    /* First, sort the methods into decreasing order of inclusive
     * elapsed time so that we can assign the method indices.
     */
    qsort(pMethods, numMethods, sizeof(MethodEntry*), compareElapsedInclusive);

    for (ii = 0; ii < numMethods; ++ii)
        pMethods[ii]->index = ii;

    /* Sort the methods into decreasing order of exclusive elapsed time.
     */
    qsort(pMethods, numMethods, sizeof(MethodEntry*),
          compareElapsedExclusive);

    printf("Total cycles: %llu\n\n", sumThreadTime);
    if (gOptions.outputHtml) {
        printf("<br><br>\n");
    }
    printf("Exclusive elapsed times for each method, not including time spent in\n");
    printf("children, sorted by exclusive time.\n\n");
    if (gOptions.outputHtml) {
        printf("<br><br>\n<pre>\n");
    }

    printf("    Usecs  self %%  sum %%  Method\n");
    sum = 0;

    for (ii = 0; ii < numMethods; ++ii) {
        char *className, *methodName, *signature;

        method = pMethods[ii];
        /* Don't show methods with zero cycles */
        if (method->elapsedExclusive == 0)
            break;
        className = (char*)(method->className);
        methodName = (char*)(method->methodName);
        signature = (char*)(method->signature);
        sum += method->elapsedExclusive;
        per = 100.0 * method->elapsedExclusive / total;
        sum_per = 100.0 * sum / total;
        if (gOptions.outputHtml) {
            sprintf(anchor_buf, "<a href=\"#m%d\">", method->index);
            className = htmlEscape(className, classBuf, HTML_BUFSIZE);
            methodName = htmlEscape(methodName, methodBuf, HTML_BUFSIZE);
            signature = htmlEscape(signature, signatureBuf, HTML_BUFSIZE);
        }
        if (method->methodName) {
            printf("%9llu  %6.2f %6.2f  %s[%d]%s %s.%s %s\n",
                   method->elapsedExclusive, per, sum_per,
                   anchor_buf, method->index, anchor_close,
                   className, methodName, signature);
        } else {
            printf("%9llu  %6.2f %6.2f  %s[%d]%s %s\n",
                   method->elapsedExclusive, per, sum_per,
                   anchor_buf, method->index, anchor_close,
                   className);
        }
    }
    if (gOptions.outputHtml) {
        printf("</pre>\n");
    }
}

/* check to make sure that the child method meets the threshold of the parent */
int checkThreshold(MethodEntry* parent, MethodEntry* child)
{
    double parentTime = parent->elapsedInclusive;
    double childTime = child->elapsedInclusive;
    int64_t percentage = (childTime / parentTime) * 100.0;
    return (percentage < gOptions.threshold) ? 0 : 1;
}

void createLabels(FILE* file, MethodEntry* method)
{
    fprintf(file, "node%d[label = \"[%d] %s.%s (%llu, %llu, %d)\"]\n",
             method->index, method->index, method->className, method->methodName,
             method->elapsedInclusive / 1000,
             method->elapsedExclusive / 1000,
             method->numCalls[0]);

    method->graphState = GRAPH_LABEL_VISITED;

    TimedMethod* child;
    for (child = method->children[0] ; child ; child = child->next) {
        MethodEntry* childMethod = child->method;

        if ((childMethod->graphState & GRAPH_LABEL_VISITED) == 0 && checkThreshold(method, childMethod)) {
            createLabels(file, child->method);
        }
    }
}

void createLinks(FILE* file, MethodEntry* method)
{
    method->graphState |= GRAPH_NODE_VISITED;

    TimedMethod* child;
    for (child = method->children[0] ; child ; child = child->next) {
        MethodEntry* childMethod = child->method;
        if (checkThreshold(method, child->method)) {
            fprintf(file, "node%d -> node%d\n", method->index, child->method->index);
            // only visit children that haven't been visited before
            if ((childMethod->graphState & GRAPH_NODE_VISITED) == 0) {
                createLinks(file, child->method);
            }
        }
    }
}

void createInclusiveProfileGraphNew(DataKeys* dataKeys)
{
    // create a temporary file in /tmp
    char path[FILENAME_MAX];
    if (gOptions.keepDotFile) {
        snprintf(path, FILENAME_MAX, "%s.dot", gOptions.graphFileName);
    } else {
        snprintf(path, FILENAME_MAX, "/tmp/dot-%d-%d.dot", (int)time(NULL), rand());
    }

    FILE* file = fopen(path, "w+");

    fprintf(file, "digraph g {\nnode [shape = record,height=.1];\n");

    createLabels(file, dataKeys->methods);
    createLinks(file, dataKeys->methods);

    fprintf(file, "}");
    fclose(file);

    // now that we have the dot file generate the image
    char command[1024];
    snprintf(command, 1024, "dot -Tpng -o '%s' '%s'", gOptions.graphFileName, path);

    system(command);

    if (! gOptions.keepDotFile) {
        remove(path);
    }
}

void printInclusiveProfile(MethodEntry **pMethods, int numMethods,
                           uint64_t sumThreadTime)
{
    int ii;
    MethodEntry* method;
    double total, sum, per, sum_per;
    char classBuf[HTML_BUFSIZE], methodBuf[HTML_BUFSIZE];
    char signatureBuf[HTML_BUFSIZE];
    char anchor_buf[80];

    total = sumThreadTime;
    anchor_buf[0] = 0;
    if (gOptions.outputHtml) {
        printf("<a name=\"inclusive\"></a>\n");
        printf("<hr>\n");
        outputNavigationBar();
    } else {
        printf("\n%s\n", profileSeparator);
    }

    /* Sort the methods into decreasing order of inclusive elapsed time. */
    qsort(pMethods, numMethods, sizeof(MethodEntry*),
          compareElapsedInclusive);

    printf("\nInclusive elapsed times for each method and its parents and children,\n");
    printf("sorted by inclusive time.\n\n");

    if (gOptions.outputHtml) {
        printf("<br><br>\n<pre>\n");
    }

    printf("index  %%/total %%/self  index     calls         usecs name\n");
    for (ii = 0; ii < numMethods; ++ii) {
        int num;
        TimedMethod *pTimed;
        double excl_per;
        char buf[40];
        char *className, *methodName, *signature;

        method = pMethods[ii];
        /* Don't show methods with zero cycles */
        if (method->elapsedInclusive == 0)
            break;

        className = (char*)(method->className);
        methodName = (char*)(method->methodName);
        signature = (char*)(method->signature);

        if (gOptions.outputHtml) {
            printf("<a name=\"m%d\"></a>", method->index);
            className = htmlEscape(className, classBuf, HTML_BUFSIZE);
            methodName = htmlEscape(methodName, methodBuf, HTML_BUFSIZE);
            signature = htmlEscape(signature, signatureBuf, HTML_BUFSIZE);
        }
        printf("----------------------------------------------------\n");

        /* Sort and print the parents */
        int numCalls = method->numCalls[0] + method->numCalls[1];
        printInclusiveMethod(method, method->parents[0], numCalls, 0);
        if (method->parents[1]) {
            printf("               +++++++++++++++++++++++++\n");
            printInclusiveMethod(method, method->parents[1], numCalls,
                                 kIsRecursive);
        }

        per = 100.0 * method->elapsedInclusive / total;
        sprintf(buf, "[%d]", ii);
        if (method->methodName) {
            printf("%-6s %5.1f%%   %5s %6s %6d+%-6d %9llu %s.%s %s\n",
                   buf,
                   per, "", "", method->numCalls[0], method->numCalls[1],
                   method->elapsedInclusive,
                   className, methodName, signature);
        } else {
            printf("%-6s %5.1f%%   %5s %6s %6d+%-6d %9llu %s\n",
                   buf,
                   per, "", "", method->numCalls[0], method->numCalls[1],
                   method->elapsedInclusive,
                   className);
        }
        excl_per = 100.0 * method->topExclusive / method->elapsedInclusive;
        printf("%6s %5s   %5.1f%% %6s %6s %6s %9llu\n",
               "", "", excl_per, "excl", "", "", method->topExclusive);

        /* Sort and print the children */
        printInclusiveMethod(method, method->children[0], 0, 0);
        if (method->children[1]) {
            printf("               +++++++++++++++++++++++++\n");
            printInclusiveMethod(method, method->children[1], 0,
                                 kIsRecursive);
        }
    }
    if (gOptions.outputHtml) {
        printf("</pre>\n");
    }
}

void printThreadProfile(ThreadEntry *pThreads, int numThreads, uint64_t sumThreadTime, Filter** filters)
{
    int ii, jj;
    ThreadEntry thread;
    double total, per, sum_per;
    uint64_t sum;
    char threadBuf[HTML_BUFSIZE];
    char anchor_buf[80];
    int drawTable;

    total = sumThreadTime;
    anchor_buf[0] = 0;
    if (gOptions.outputHtml) {
        printf("<a name=\"thread\"></a>\n");
        printf("<hr>\n");
        outputNavigationBar();
    } else {
        printf("\n%s\n", profileSeparator);
    }

    /* Sort the threads into decreasing order of elapsed time. */
    qsort(pThreads, numThreads, sizeof(ThreadEntry), compareElapsed);

    printf("\nElapsed times for each thread, sorted by elapsed time.\n");
    printf("Also includes percentage of time spent during the <i>execution</i> of any filters.\n\n");

    if (gOptions.outputHtml) {
        printf("<br><br>\n<pre>\n");
    }

    printf("    Usecs   self %%  sum %%");
    for (ii = 0; ii < numFilters; ++ii) {
        printf("  %s %%", filters[ii]->filterName);
    }
    printf("  tid   ThreadName\n");
    sum = 0;

    for (ii = 0; ii < numThreads; ++ii) {
        int threadId;
        char *threadName;
        uint64_t time;

        thread = pThreads[ii];

        threadId = thread.threadId;
        threadName = (char*)(thread.threadName);
        time = thread.elapsedTime;

        sum += time;
        per = 100.0 * time / total;
        sum_per = 100.0 * sum / total;

        if (gOptions.outputHtml) {
	    threadName = htmlEscape(threadName, threadBuf, HTML_BUFSIZE);
        }

	printf("%9llu  %6.2f %6.2f", time, per, sum_per);
	for (jj = 0; jj < numFilters; jj++) {
	    printf(" %6.2f", 100.0 * filters[jj]->times.threadExecutionTimes[threadId] / time);
	}
	printf("    %3d %s\n", threadId, threadName);
    }

    if (gOptions.outputHtml)
        printf("</pre><br />");

    printf("\n\nBreak-down of portion of time spent by each thread while waiting on a filter method.\n");

    for (ii = 0; ii < numFilters; ++ii) {
        // Draw a table for each filter that measures wait time
        drawTable = 0;
	for (jj = 0; jj < filters[ii]->numKeys; jj++)
	    if (filters[ii]->filterKeys[jj].flags == 1)
	        drawTable = 1;

	if (drawTable) {

	    if (gOptions.outputHtml)
	        printf("<br/><br/>\n<pre>\n");
	    printf("Filter: %s\n", filters[ii]->filterName);
	    printf("Total waiting cycles: %llu (%6.2f%% of total)\n",
		   filters[ii]->times.totalWaitTime,
		   100.0 * filters[ii]->times.totalWaitTime / sum);

	    if (filters[ii]->times.totalWaitTime > 0) {

	        printf("Details: \n\n");

		printf(" Waiting cycles    %% of total waiting time   execution time while waiting    thread name\n");

		for (jj = 0; jj < numThreads; jj++) {

		    thread = pThreads[jj];

		    char *threadName;
		    threadName = (char*) thread.threadName;
		    if (gOptions.outputHtml) {
		        threadName = htmlEscape(threadName, threadBuf, HTML_BUFSIZE);
		    }

		    printf(" %9llu                   %6.2f                     %6.2f               %s\n",
			   filters[ii]->times.threadWaitTimes[thread.threadId],
			   100.0 * filters[ii]->times.threadWaitTimes[thread.threadId] / filters[ii]->times.totalWaitTime,
			   100.0 * filters[ii]->times.threadExecutionTimesWhileWaiting[thread.threadId] / filters[ii]->times.totalWaitTime,
			   threadName);
		}
	    }

	    if (gOptions.outputHtml)
	        printf("</pre>\n");

	}
    }

}

void createClassList(TraceData* traceData, MethodEntry **pMethods, int numMethods)
{
    int ii;

    /* Sort the methods into alphabetical order to find the unique class
     * names.
     */
    qsort(pMethods, numMethods, sizeof(MethodEntry*), compareClassNames);

    /* Count the number of unique class names. */
    const char *currentClassName = "";
    const char *firstClassName = NULL;
    traceData->numClasses = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL) {
            continue;
        }
        if (strcmp(pMethods[ii]->className, currentClassName) != 0) {
            // Remember the first one
            if (firstClassName == NULL) {
                firstClassName = pMethods[ii]->className;
            }
            traceData->numClasses += 1;
            currentClassName = pMethods[ii]->className;
        }
    }

    if (traceData->numClasses == 0) {
        traceData->classes = NULL;
        return;
    }

    /* Allocate space for all of the unique class names */
    traceData->classes = (ClassEntry *) malloc(sizeof(ClassEntry) * traceData->numClasses);

    /* Initialize the classes array */
    memset(traceData->classes, 0, sizeof(ClassEntry) * traceData->numClasses);
    ClassEntry *pClass = traceData->classes;
    pClass->className = currentClassName = firstClassName;
    int prevNumMethods = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL) {
            continue;
        }
        if (strcmp(pMethods[ii]->className, currentClassName) != 0) {
            pClass->numMethods = prevNumMethods;
            (++pClass)->className = currentClassName = pMethods[ii]->className;
            prevNumMethods = 0;
        }
        prevNumMethods += 1;
    }
    pClass->numMethods = prevNumMethods;

    /* Create the array of MethodEntry pointers for each class */
    pClass = NULL;
    currentClassName = "";
    int nextMethod = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL) {
            continue;
        }
        if (strcmp(pMethods[ii]->className, currentClassName) != 0) {
            currentClassName = pMethods[ii]->className;
            if (pClass == NULL)
                pClass = traceData->classes;
            else
                pClass++;
            /* Allocate space for the methods array */
            int nbytes = sizeof(MethodEntry*) * pClass->numMethods;
            pClass->methods = (MethodEntry**) malloc(nbytes);
            nextMethod = 0;
        }
        pClass->methods[nextMethod++] = pMethods[ii];
    }
}

/* Prints a number of html non-breaking spaces according so that the length
 * of the string "buf" is at least "width" characters wide.  If width is
 * negative, then trailing spaces are added instead of leading spaces.
 */
void printHtmlField(char *buf, int width)
{
    int ii;

    int leadingSpaces = 1;
    if (width < 0) {
        width = -width;
        leadingSpaces = 0;
    }
    int len = strlen(buf);
    int numSpaces = width - len;
    if (numSpaces <= 0) {
        printf("%s", buf);
        return;
    }
    if (leadingSpaces == 0)
        printf("%s", buf);
    for (ii = 0; ii < numSpaces; ++ii)
        printf("&nbsp;");
    if (leadingSpaces == 1)
        printf("%s", buf);
}

void printClassProfiles(TraceData* traceData, uint64_t sumThreadTime)
{
    int ii, jj;
    MethodEntry* method;
    double total, sum, per, sum_per;
    char classBuf[HTML_BUFSIZE], methodBuf[HTML_BUFSIZE];
    char signatureBuf[HTML_BUFSIZE];

    total = sumThreadTime;
    if (gOptions.outputHtml) {
        printf("<a name=\"class\"></a>\n");
        printf("<hr>\n");
        outputNavigationBar();
    } else {
        printf("\n%s\n", profileSeparator);
    }

    if (traceData->numClasses == 0) {
        printf("\nNo classes.\n");
        if (gOptions.outputHtml) {
            printf("<br><br>\n");
        }
        return;
    }

    printf("\nExclusive elapsed time for each class, summed over all the methods\n");
    printf("in the class.\n\n");
    if (gOptions.outputHtml) {
        printf("<br><br>\n");
    }

    /* For each class, sum the exclusive times in all of the methods
     * in that class.  Also sum the number of method calls.  Also
     * sort the methods so the most expensive appear at the top.
     */
    ClassEntry *pClass = traceData->classes;
    for (ii = 0; ii < traceData->numClasses; ++ii, ++pClass) {
        //printf("%s %d methods\n", pClass->className, pClass->numMethods);
        int numMethods = pClass->numMethods;
        for (jj = 0; jj < numMethods; ++jj) {
            method = pClass->methods[jj];
            pClass->elapsedExclusive += method->elapsedExclusive;
            pClass->numCalls[0] += method->numCalls[0];
            pClass->numCalls[1] += method->numCalls[1];
        }

        /* Sort the methods into decreasing order of exclusive time */
        qsort(pClass->methods, numMethods, sizeof(MethodEntry*),
              compareElapsedExclusive);
    }

    /* Allocate an array of pointers to the classes for more efficient
     * sorting.
     */
    ClassEntry **pClasses;
    pClasses = (ClassEntry**) malloc(sizeof(ClassEntry*) * traceData->numClasses);
    for (ii = 0; ii < traceData->numClasses; ++ii)
        pClasses[ii] = &traceData->classes[ii];

    /* Sort the classes into decreasing order of exclusive time */
    qsort(pClasses, traceData->numClasses, sizeof(ClassEntry*), compareClassExclusive);

    if (gOptions.outputHtml) {
        printf("<div class=\"header\"><span class=\"parent\">&nbsp;</span>&nbsp;&nbsp;&nbsp;");
        printf("Cycles %%/total Cumul.%% &nbsp;Calls+Recur&nbsp; Class</div>\n");
    } else {
        printf("   Cycles %%/total Cumul.%%  Calls+Recur  Class\n");
    }

    sum = 0;
    for (ii = 0; ii < traceData->numClasses; ++ii) {
        char *className, *methodName, *signature;

        /* Skip classes with zero cycles */
        pClass = pClasses[ii];
        if (pClass->elapsedExclusive == 0)
            break;

        per = 100.0 * pClass->elapsedExclusive / total;
        sum += pClass->elapsedExclusive;
        sum_per = 100.0 * sum / total;
        className = (char*)(pClass->className);
        if (gOptions.outputHtml) {
            char buf[80];

            className = htmlEscape(className, classBuf, HTML_BUFSIZE);
            printf("<div class=\"link\" onClick=\"javascript:toggle('d%d')\" onMouseOver=\"javascript:onMouseOver(this)\" onMouseOut=\"javascript:onMouseOut(this)\"><span class=\"parent\" id=\"xd%d\">+</span>", ii, ii);
            sprintf(buf, "%llu", pClass->elapsedExclusive);
            printHtmlField(buf, 9);
            printf(" ");
            sprintf(buf, "%.1f", per);
            printHtmlField(buf, 7);
            printf(" ");
            sprintf(buf, "%.1f", sum_per);
            printHtmlField(buf, 7);
            printf(" ");
            sprintf(buf, "%d", pClass->numCalls[0]);
            printHtmlField(buf, 6);
            printf("+");
            sprintf(buf, "%d", pClass->numCalls[1]);
            printHtmlField(buf, -6);
            printf(" ");
            printf("%s", className);
            printf("</div>\n");
            printf("<div class=\"parent\" id=\"d%d\">\n", ii);
        } else {
            printf("---------------------------------------------\n");
            printf("%9llu %7.1f %7.1f %6d+%-6d %s\n",
                   pClass->elapsedExclusive, per, sum_per,
                   pClass->numCalls[0], pClass->numCalls[1],
                   className);
        }

        int numMethods = pClass->numMethods;
        double classExclusive = pClass->elapsedExclusive;
        double sumMethods = 0;
        for (jj = 0; jj < numMethods; ++jj) {
            method = pClass->methods[jj];
            methodName = (char*)(method->methodName);
            signature = (char*)(method->signature);
            per = 100.0 * method->elapsedExclusive / classExclusive;
            sumMethods += method->elapsedExclusive;
            sum_per = 100.0 * sumMethods / classExclusive;
            if (gOptions.outputHtml) {
                char buf[80];

                methodName = htmlEscape(methodName, methodBuf, HTML_BUFSIZE);
                signature = htmlEscape(signature, signatureBuf, HTML_BUFSIZE);
                printf("<div class=\"leaf\"><span class=\"leaf\">&nbsp;</span>");
                sprintf(buf, "%llu", method->elapsedExclusive);
                printHtmlField(buf, 9);
                printf("&nbsp;");
                sprintf(buf, "%llu", method->elapsedInclusive);
                printHtmlField(buf, 9);
                printf("&nbsp;");
                sprintf(buf, "%.1f", per);
                printHtmlField(buf, 7);
                printf("&nbsp;");
                sprintf(buf, "%.1f", sum_per);
                printHtmlField(buf, 7);
                printf("&nbsp;");
                sprintf(buf, "%d", method->numCalls[0]);
                printHtmlField(buf, 6);
                printf("+");
                sprintf(buf, "%d", method->numCalls[1]);
                printHtmlField(buf, -6);
                printf("&nbsp;");
                printf("<a href=\"#m%d\">[%d]</a>&nbsp;%s&nbsp;%s",
                       method->index, method->index, methodName, signature);
                printf("</div>\n");
            } else {
                printf("%9llu %9llu %7.1f %7.1f %6d+%-6d [%d] %s %s\n",
                       method->elapsedExclusive,
                       method->elapsedInclusive,
                       per, sum_per,
                       method->numCalls[0], method->numCalls[1],
                       method->index, methodName, signature);
            }
        }
        if (gOptions.outputHtml) {
            printf("</div>\n");
        }
    }
}

void createUniqueMethodList(TraceData* traceData, MethodEntry **pMethods, int numMethods)
{
    int ii;

    /* Sort the methods into alphabetical order of method names
     * to find the unique method names.
     */
    qsort(pMethods, numMethods, sizeof(MethodEntry*), compareMethodNames);

    /* Count the number of unique method names, ignoring class and
     * signature.
     */
    const char *currentMethodName = "";
    traceData->numUniqueMethods = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL)
            continue;
        if (strcmp(pMethods[ii]->methodName, currentMethodName) != 0) {
            traceData->numUniqueMethods += 1;
            currentMethodName = pMethods[ii]->methodName;
        }
    }
    if (traceData->numUniqueMethods == 0)
        return;

    /* Allocate space for pointers to all of the unique methods */
    int nbytes = sizeof(UniqueMethodEntry) * traceData->numUniqueMethods;
    traceData->uniqueMethods = (UniqueMethodEntry *) malloc(nbytes);

    /* Initialize the uniqueMethods array */
    memset(traceData->uniqueMethods, 0, nbytes);
    UniqueMethodEntry *pUnique = traceData->uniqueMethods;
    currentMethodName = NULL;
    int prevNumMethods = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL)
            continue;
        if (currentMethodName == NULL)
            currentMethodName = pMethods[ii]->methodName;
        if (strcmp(pMethods[ii]->methodName, currentMethodName) != 0) {
            currentMethodName = pMethods[ii]->methodName;
            pUnique->numMethods = prevNumMethods;
            pUnique++;
            prevNumMethods = 0;
        }
        prevNumMethods += 1;
    }
    pUnique->numMethods = prevNumMethods;

    /* Create the array of MethodEntry pointers for each unique method */
    pUnique = NULL;
    currentMethodName = "";
    int nextMethod = 0;
    for (ii = 0; ii < numMethods; ++ii) {
        if (pMethods[ii]->methodName == NULL)
            continue;
        if (strcmp(pMethods[ii]->methodName, currentMethodName) != 0) {
            currentMethodName = pMethods[ii]->methodName;
            if (pUnique == NULL)
                pUnique = traceData->uniqueMethods;
            else
                pUnique++;
            /* Allocate space for the methods array */
            int nbytes = sizeof(MethodEntry*) * pUnique->numMethods;
            pUnique->methods = (MethodEntry**) malloc(nbytes);
            nextMethod = 0;
        }
        pUnique->methods[nextMethod++] = pMethods[ii];
    }
}

void printMethodProfiles(TraceData* traceData, uint64_t sumThreadTime)
{
    int ii, jj;
    MethodEntry* method;
    double total, sum, per, sum_per;
    char classBuf[HTML_BUFSIZE], methodBuf[HTML_BUFSIZE];
    char signatureBuf[HTML_BUFSIZE];

    if (traceData->numUniqueMethods == 0)
        return;

    total = sumThreadTime;
    if (gOptions.outputHtml) {
        printf("<a name=\"method\"></a>\n");
        printf("<hr>\n");
        outputNavigationBar();
    } else {
        printf("\n%s\n", profileSeparator);
    }

    printf("\nExclusive elapsed time for each method, summed over all the classes\n");
    printf("that contain a method with the same name.\n\n");
    if (gOptions.outputHtml) {
        printf("<br><br>\n");
    }

    /* For each unique method, sum the exclusive times in all of the methods
     * with the same name.  Also sum the number of method calls.  Also
     * sort the methods so the most expensive appear at the top.
     */
    UniqueMethodEntry *pUnique = traceData->uniqueMethods;
    for (ii = 0; ii < traceData->numUniqueMethods; ++ii, ++pUnique) {
        int numMethods = pUnique->numMethods;
        for (jj = 0; jj < numMethods; ++jj) {
            method = pUnique->methods[jj];
            pUnique->elapsedExclusive += method->elapsedExclusive;
            pUnique->numCalls[0] += method->numCalls[0];
            pUnique->numCalls[1] += method->numCalls[1];
        }

        /* Sort the methods into decreasing order of exclusive time */
        qsort(pUnique->methods, numMethods, sizeof(MethodEntry*),
              compareElapsedExclusive);
    }

    /* Allocate an array of pointers to the methods for more efficient
     * sorting.
     */
    UniqueMethodEntry **pUniqueMethods;
    int nbytes = sizeof(UniqueMethodEntry*) * traceData->numUniqueMethods;
    pUniqueMethods = (UniqueMethodEntry**) malloc(nbytes);
    for (ii = 0; ii < traceData->numUniqueMethods; ++ii)
        pUniqueMethods[ii] = &traceData->uniqueMethods[ii];

    /* Sort the methods into decreasing order of exclusive time */
    qsort(pUniqueMethods, traceData->numUniqueMethods, sizeof(UniqueMethodEntry*),
          compareUniqueExclusive);

    if (gOptions.outputHtml) {
        printf("<div class=\"header\"><span class=\"parent\">&nbsp;</span>&nbsp;&nbsp;&nbsp;");
        printf("Cycles %%/total Cumul.%% &nbsp;Calls+Recur&nbsp; Method</div>\n");
    } else {
        printf("   Cycles %%/total Cumul.%%  Calls+Recur  Method\n");
    }

    sum = 0;
    for (ii = 0; ii < traceData->numUniqueMethods; ++ii) {
        char *className, *methodName, *signature;

        /* Skip methods with zero cycles */
        pUnique = pUniqueMethods[ii];
        if (pUnique->elapsedExclusive == 0)
            break;

        per = 100.0 * pUnique->elapsedExclusive / total;
        sum += pUnique->elapsedExclusive;
        sum_per = 100.0 * sum / total;
        methodName = (char*)(pUnique->methods[0]->methodName);
        if (gOptions.outputHtml) {
            char buf[80];

            methodName = htmlEscape(methodName, methodBuf, HTML_BUFSIZE);
            printf("<div class=\"link\" onClick=\"javascript:toggle('e%d')\" onMouseOver=\"javascript:onMouseOver(this)\" onMouseOut=\"javascript:onMouseOut(this)\"><span class=\"parent\" id=\"xe%d\">+</span>", ii, ii);
            sprintf(buf, "%llu", pUnique->elapsedExclusive);
            printHtmlField(buf, 9);
            printf(" ");
            sprintf(buf, "%.1f", per);
            printHtmlField(buf, 7);
            printf(" ");
            sprintf(buf, "%.1f", sum_per);
            printHtmlField(buf, 7);
            printf(" ");
            sprintf(buf, "%d", pUnique->numCalls[0]);
            printHtmlField(buf, 6);
            printf("+");
            sprintf(buf, "%d", pUnique->numCalls[1]);
            printHtmlField(buf, -6);
            printf(" ");
            printf("%s", methodName);
            printf("</div>\n");
            printf("<div class=\"parent\" id=\"e%d\">\n", ii);
        } else {
            printf("---------------------------------------------\n");
            printf("%9llu %7.1f %7.1f %6d+%-6d %s\n",
                   pUnique->elapsedExclusive, per, sum_per,
                   pUnique->numCalls[0], pUnique->numCalls[1],
                   methodName);
        }
        int numMethods = pUnique->numMethods;
        double methodExclusive = pUnique->elapsedExclusive;
        double sumMethods = 0;
        for (jj = 0; jj < numMethods; ++jj) {
            method = pUnique->methods[jj];
            className = (char*)(method->className);
            signature = (char*)(method->signature);
            per = 100.0 * method->elapsedExclusive / methodExclusive;
            sumMethods += method->elapsedExclusive;
            sum_per = 100.0 * sumMethods / methodExclusive;
            if (gOptions.outputHtml) {
                char buf[80];

                className = htmlEscape(className, classBuf, HTML_BUFSIZE);
                signature = htmlEscape(signature, signatureBuf, HTML_BUFSIZE);
                printf("<div class=\"leaf\"><span class=\"leaf\">&nbsp;</span>");
                sprintf(buf, "%llu", method->elapsedExclusive);
                printHtmlField(buf, 9);
                printf("&nbsp;");
                sprintf(buf, "%llu", method->elapsedInclusive);
                printHtmlField(buf, 9);
                printf("&nbsp;");
                sprintf(buf, "%.1f", per);
                printHtmlField(buf, 7);
                printf("&nbsp;");
                sprintf(buf, "%.1f", sum_per);
                printHtmlField(buf, 7);
                printf("&nbsp;");
                sprintf(buf, "%d", method->numCalls[0]);
                printHtmlField(buf, 6);
                printf("+");
                sprintf(buf, "%d", method->numCalls[1]);
                printHtmlField(buf, -6);
                printf("&nbsp;");
                printf("<a href=\"#m%d\">[%d]</a>&nbsp;%s.%s&nbsp;%s",
                       method->index, method->index,
                       className, methodName, signature);
                printf("</div>\n");
            } else {
                printf("%9llu %9llu %7.1f %7.1f %6d+%-6d [%d] %s.%s %s\n",
                       method->elapsedExclusive,
                       method->elapsedInclusive,
                       per, sum_per,
                       method->numCalls[0], method->numCalls[1],
                       method->index, className, methodName, signature);
            }
        }
        if (gOptions.outputHtml) {
            printf("</div>\n");
        }
    }
}

/*
 * Determines whether the given FilterKey matches the method. The FilterKey's
 * key that is used to match against the method is determined by index.
 */
int keyMatchesMethod(FilterKey filterKey, MethodEntry* method, int index)
{
    if (filterKey.type[index] == 0) { // Class
#if 0
        fprintf(stderr, "  class is %s; filter key is %s\n", method->className, filterKey.keys[index]);
#endif
        if (strcmp(method->className, filterKey.keys[index]) == 0) {
	    return 1;
	}
    } else { // Method
        if (method->methodName != NULL) {
	    // Get fully-qualified name
            // TODO: parse class name and method name an put them in structure to avoid
            // allocating memory here
	    char* str = malloc ((strlen(method->className) + strlen(method->methodName) + 2) * sizeof(char));
	    strcpy(str, method->className);
	    strcat(str, ".");
	    strcat(str, method->methodName);
#if 0
	    fprintf(stderr, "  method is %s; filter key is %s\n", str, filterKey.keys[index]);
#endif
	    if (strcmp(str, filterKey.keys[index]) == 0) {
	        free(str);
	        return 1;
	    }
	    free(str);
	}
    }
    return 0;
}

/*
 * Adds the appropriate times to the given filter based on the given method. Activates and
 * de-activates filters as necessary.
 *
 * A filter is activated when the given method matches the 'entry' key of one of its FilterKeys.
 * It is de-activated when the method matches the 'exit' key of the same FilterKey that activated it
 * in the first place. Thus, a filter may be active more than once on the same thread (activated by
 * different FilterKeys). A filter may also be active on different threads at the same time.
 *
 * While the filter is active on thread 1, elapsed time is allocated to different buckets which
 * include: thread execution time (i.e., time thread 1 spent executing while filter was active),
 * thread waiting time (i.e., time thread 1 waited while other threads executed), and execution
 * time while waiting (i.e., time thread x spent executing while thread 1 was waiting). We also
 * keep track of the total waiting time for a given filter.
 *
 * Lastly, we keep track of remaining (un-allocated) time for cases in which we exit a method we
 * had not entered before, and that method happens to match the 'exit' key of a FilterKey.
 */
int filterMethod(MethodEntry* method, Filter* filter, int entry, int threadId, int numThreads,
		 uint64_t elapsed, uint64_t remTime)
{
    int ii, jj;
    int activeCount, addedWaitTimeThreadsCount;
    int* activeThreads;
    int* activationKeys;
    int* addedWaitTimeThreads;

    // flags
    int addWaitTime = 0;
    int deactivation = 0;
    int addedExecutionTime = 0;
    int addedExecutionTimeWhileWaiting = 0;
    int addedWaitTime;
    int addedRemTime = 0;
    int threadKeyPairActive = 0;

    if (filter->times.threadWaitTimes == NULL && filter->times.threadExecutionTimes == NULL &&
	filter->times.threadExecutionTimesWhileWaiting == NULL) {
        filter->times.threadWaitTimes = (uint64_t*) calloc(MAX_THREADS, sizeof(uint64_t));
	filter->times.threadExecutionTimesWhileWaiting =
          (uint64_t*) calloc(MAX_THREADS, sizeof(uint64_t));
	filter->times.threadExecutionTimes = (uint64_t*) calloc(MAX_THREADS, sizeof(uint64_t));
    }

    int verbose = 0;

    if (verbose)
        fprintf(stderr,
                "Running %s filter for class %s method %s, thread %d; activeCount: %d time: %llu\n",
                filter->filterName, method->className, method->methodName, threadId,
                filter->activeCount, elapsed);

    // If active on some thread
    if (filter->activeCount > 0) {

        // Initialize active structures in case there are any de-activations
        activeThreads = (int*) calloc(filter->activeCount, sizeof(int));
	activationKeys = (int*) calloc(filter->activeCount, sizeof(int));
	activeCount = 0;

	// Initialize structure to help us determine which threads we've already added wait time to
	addedWaitTimeThreads = (int*) calloc(filter->activeCount, sizeof(int));
	addedWaitTimeThreadsCount = 0;

        // Add times to appropriate sums and de-activate (if necessary)
        for (ii = 0; ii < filter->activeCount; ii++) {

	    if (verbose) {
	        fprintf(stderr, "  Analyzing active thread with id %d, activated by key [%s, %s]\n",
			filter->activeThreads[ii],
                        filter->filterKeys[filter->activationKeys[ii]].keys[0],
			filter->filterKeys[filter->activationKeys[ii]].keys[1]);
	    }

	    // If active on THIS thread -> add to execution time (only add once!)
	    if (filter->activeThreads[ii] == threadId && !addedExecutionTime) {
	        if (verbose)
		    fprintf(stderr, "  Adding execution time to this thead\n");
	        filter->times.threadExecutionTimes[threadId] += elapsed;
		addedExecutionTime = 1;
	    }

	    // If active on ANOTHER thread (or this one too) with CROSS_THREAD_FLAG -> add to
            // both thread's waiting time + total
	    if (filter->filterKeys[filter->activationKeys[ii]].flags == 1) {

	        // Add time to thread that is waiting (add to each waiting thread at most once!)
	        addedWaitTime = 0;
		for (jj = 0; jj < addedWaitTimeThreadsCount; jj++) {
		    if (addedWaitTimeThreads[jj] == filter->activeThreads[ii])
		        addedWaitTime = 1;
		}
	        if (!addedWaitTime) {
		    if (verbose)
		        fprintf(stderr, "  Adding wait time to waiting thread\n");
		    filter->times.threadWaitTimes[filter->activeThreads[ii]] += elapsed;
		    addedWaitTimeThreads[addedWaitTimeThreadsCount++] = filter->activeThreads[ii];
		}

                // Add execution time to this thread while the other is waiting (only add once!)
                // [Flag is needed only because outside for loop might iterate through same
                // thread twice?] TODO: verify
		if (!addedExecutionTimeWhileWaiting) {
		    if (verbose)
		        fprintf(stderr, "  Adding exec time to this thread while thread waits\n");
		    filter->times.threadExecutionTimesWhileWaiting[threadId] += elapsed;
		    addedExecutionTimeWhileWaiting = 1;
		}

		addWaitTime = 1;
	    }

	    // If a method exit matches the EXIT method of an ACTIVE key -> de-activate
            // the KEY (not the entire filter!!)
	    if (!entry && keyMatchesMethod(filter->filterKeys[filter->activationKeys[ii]],
					   method, 1)) {
	        if (verbose)
		    fprintf(stderr, "  Exit key matched!\n");

	        // Deactivate by removing (NOT adding) entries from activeThreads and activationKeys
	        deactivation = 1; // singal that lists should be replaced
	    } else {
	        // No de-activation -> copy old entries into new lists
	        activeThreads[activeCount] = filter->activeThreads[ii];
		activationKeys[activeCount++] = filter->activationKeys[ii];
	    }
	}

	// If waiting on ANY thread, add wait time to total (but only ONCE!)
	if (addWaitTime) {
	    filter->times.totalWaitTime += elapsed;
	}

	// If de-activation occurred, replace lists
	if (deactivation) {
	    // TODO: Free memory from old lists

	    // Set new lists
	    filter->activeThreads = activeThreads;
	    filter->activationKeys = activationKeys;
	    filter->activeCount = activeCount;
	} else {
	    // TODO: Free memory from new lists
	}

    }  // Else, continue (we might be activating the filter on a different thread)


    if (entry) { // ENTRY
        if (verbose)
	    fprintf(stderr, "  Here at the entry\n");
        // If method matches entry key -> activate thread (do not add time since it's a new entry!)
        for (ii = 0; ii < filter->numKeys; ii++) {
	    if (keyMatchesMethod(filter->filterKeys[ii], method, 0)) {
	        if (verbose)
		    fprintf(stderr, "  Entry key matched!\n");
	        // Activate thread only if thread/key pair is not already active
	        for (jj = 0; jj < filter->activeCount; jj++) {
		    if (filter->activeThreads[jj] == threadId && filter->activationKeys[jj] == ii)
		        threadKeyPairActive = 1;
		}
	        // TODO: WORRY ABOUT MEMORY WHEN ACTIVE_COUNT > DEFAULT_ACTIVE_THREAD (unlikely)
	        // TODO: what if the same thread is active multiple times by different keys?
		// nothing, we just have to make sure we dont double-add, and we dont..
		if (!threadKeyPairActive) {
		    filter->activeThreads[filter->activeCount] = threadId;
		    filter->activationKeys[filter->activeCount++] = ii;
		}
	    }
	}
    } else { // EXIT
        // If method matches a terminal key -> add remTime to total (no need to active/de-activate)
        for (ii = 0; ii < filter->numKeys; ii++) {
	    if (!deactivation && keyMatchesMethod(filter->filterKeys[ii], method, 1) &&
		keyMatchesMethod(filter->filterKeys[ii], method, 0)) {
	        // Add remTime(s)
	        // TODO: think about how we should add remTimes.. should we add remTime to threads
	        // that were waiting or being waited on? for now, keep it simple and just add the
	        // execution time to the current thread.
	        filter->times.threadExecutionTimes[threadId] += remTime;
		addedRemTime = 1;
	    }
	}
    }

    return addedExecutionTime | (addedRemTime << 1);
}

void dumpFilters(Filter** filters) {
    int i;
    for (i = 0; i < numFilters; i++) {
        int j;
	fprintf(stderr, "FILTER %s\n", filters[i]->filterName);
	for (j = 0; j < filters[i]->numKeys; j++) {
	    fprintf(stderr, "Keys: %s, type %d", filters[i]->filterKeys[j].keys[0],
		    filters[i]->filterKeys[j].type[0]);
	    if (filters[i]->filterKeys[j].keys[1] != NULL) {
	        fprintf(stderr, " AND %s, type %d", filters[i]->filterKeys[j].keys[1],
			filters[i]->filterKeys[j].type[1]);
	    }
	    fprintf(stderr, "; flags: %d\n", filters[i]->filterKeys[j].flags);
	}
    }
}

/*
 * See parseFilters for required data format.
 * 'data' must point to the beginning of a filter definition.
 */
char* parseFilter(char* data, char* dataEnd, Filter** filters, int num) {

    Filter* filter;
    int next, count, i;
    int tmpOffset, tmpKeyLen;
    char* tmpKey;
    char* key1;
    char* key2;

    filter = (Filter*) malloc(sizeof(Filter));
    filter->activeCount = 0;
    filter->activeThreads = (int*) calloc(DEFAULT_ACTIVE_THREADS, sizeof(int));
    filter->activationKeys = (int*) calloc(DEFAULT_ACTIVE_THREADS, sizeof(int));

    next = findNextChar(data + 1, dataEnd - data - 1, '\n');
    if (next < 0) {
        // TODO: what should we do here?
        // End of file reached...
    }
    data[next+1] = '\0';
    filter->filterName = data + 1;
    data += next + 2; // Careful

    /*
     * Count the number of keys (one per line).
     */
    count = countLinesToChar(data, dataEnd - data, FILTER_TAG);
    if (count <= 0) {
        fprintf(stderr,
		"ERROR: failed while parsing filter %s (found %d keys)\n",
		filter->filterName, count);
	return NULL; // TODO: Should do something else
	// Could return filter with 0 keys instead (probably better to avoid random segfaults)
    }

    filter->filterKeys = (FilterKey*) malloc(sizeof(FilterKey) * count);

    /*
     * Extract all entries.
     */
    tmpOffset = 0;
    for (i = 0; i < count; i++) {
        next = findNextChar(data, dataEnd - data, '\n');
	//        assert(next > 0); // TODO: revise... (skip if next == 0 ?)
        data[next] = '\0';
	tmpKey = data;

        if (*data == FILTER_FLAG_THREAD) {
            filter->filterKeys[i].flags = 1;
            tmpKey++;
	} else {
            filter->filterKeys[i].flags = 0;
	}

	tmpOffset = findNextChar(tmpKey, next, ',');

        if (tmpOffset < 0) {
            // No comma, so only 1 key
            key1 = tmpKey;
	    key2 = tmpKey;

	    // Get type for key1
            filter->filterKeys[i].type[0] = FILTER_TYPE_CLASS; // default
            tmpOffset = findNextChar(key1, next, '(');
	    if (tmpOffset > 0) {
	        if (findNextChar(key1, next, ')') == tmpOffset + 1) {
		    filter->filterKeys[i].type[0] = FILTER_TYPE_METHOD;
		    filter->filterKeys[i].type[1] = FILTER_TYPE_METHOD;
		}
		key1[tmpOffset] = '\0';
	    }
	} else {
	    // Pair of keys
	    tmpKey[tmpOffset] = '\0';
	    key1 = tmpKey;
	    key2 = tmpKey + tmpOffset + 1;

	    // Get type for key1
	    filter->filterKeys[i].type[0] = FILTER_TYPE_CLASS;
	    tmpKeyLen = tmpOffset;
            tmpOffset = findNextChar(key1, tmpKeyLen, '(');
	    if (tmpOffset > 0) {
	        if (findNextChar(key1, tmpKeyLen, ')') == tmpOffset + 1) {
		    filter->filterKeys[i].type[0] = FILTER_TYPE_METHOD;
		}
		key1[tmpOffset] = '\0';
	    }

	    // Get type for key2
	    filter->filterKeys[i].type[1] = FILTER_TYPE_CLASS;
            tmpOffset = findNextChar(key2, next - tmpKeyLen, '(');
	    if (tmpOffset > 0) {
	        if (findNextChar(key2, next - tmpKeyLen, ')') == tmpOffset + 1) {
		    filter->filterKeys[i].type[1] = FILTER_TYPE_METHOD;
		}
		key2[tmpOffset] = '\0';
	    }
	}

	filter->filterKeys[i].keys[0] = key1;
	filter->filterKeys[i].keys[1] = key2;
        data += next+1;
    }

    filter->numKeys = count;
    filters[num] = filter;

    return data;
}

/*
 * Parses filters from given file. The file must follow the following format:
 *
 * *FilterName    <- creates a new filter with keys to follow
 * A.method()     <- key that triggers whenever A.method() enters/exit
 * Class          <- key that triggers whenever any method from Class enters/exits
 * +CrossThread   <- same as above, but keeps track of execution times accross threads
 * B.m(),C.m()    <- key that triggers filter on when B.m() enters and off when C.m() exits
 *
 * TODO: add concrete example to make things clear
 */
Filter** parseFilters(const char* filterFileName) {

    Filter** filters = NULL;
    FILE* fp = NULL;
    long len;
    char* data;
    char* dataEnd;
    char* dataStart;
    int i, next, count;

    fp = fopen(filterFileName, "r");
    if (fp == NULL)
        goto bail;

    if (fseek(fp, 0L, SEEK_END) != 0) {
        perror("fseek");
        goto bail;
    }

    len = ftell(fp);
    if (len == 0) {
        fprintf(stderr, "WARNING: Filter file is empty.\n");
        goto bail;
    }
    rewind(fp);

    data = (char*) malloc(len);
    if (data == NULL) {
        fprintf(stderr, "ERROR: unable to alloc %ld bytes for filter file\n", len);
        goto bail;
    }

    // Read file into memory
    if (fread(data, 1, len, fp) != (size_t) len) {
        fprintf(stderr, "ERROR: unable to read %ld bytes from filter file\n", len);
        goto bail;
    }

    dataStart = data;
    dataEnd = data + len;

    // Figure out how many filters there are
    numFilters = 0;
    next = -1;

    while (1) {
        if (*data == FILTER_TAG)
	    numFilters++;
        next = findNextChar(data, len, '\n');
        if (next < 0)
            break;
        data += next+1;
        len -= next+1;
    }

    if (numFilters == 0) {
        fprintf(stderr, "WARNING: no filters found. Continuing without filters\n");
        goto bail;
    }

    filters = (Filter**) calloc(numFilters, sizeof(Filter *));
    if (filters == NULL) {
        fprintf(stderr, "ERROR: unable to alloc memory for filters");
        goto bail;
    }

    data = dataStart;
    for (i = 0; i < numFilters; i++) {
        data = parseFilter(data, dataEnd, filters, i);
    }

    return filters;

bail:
    if (fp != NULL)
        fclose(fp);

    return NULL;

}


/*
 * Read the key and data files and return the MethodEntries for those files
 */
DataKeys* parseDataKeys(TraceData* traceData, const char* traceFileName,
			uint64_t* threadTime, Filter** filters)
{
    DataKeys* dataKeys = NULL;
    MethodEntry **pMethods = NULL;
    MethodEntry* method;
    FILE* dataFp = NULL;
    DataHeader dataHeader;
    int ii, jj, numThreads;
    uint64_t currentTime;
    MethodEntry* caller;

    dataFp = fopen(traceFileName, "r");
    if (dataFp == NULL)
        goto bail;

    if ((dataKeys = parseKeys(dataFp, 0)) == NULL)
       goto bail;

    if (parseDataHeader(dataFp, &dataHeader) < 0)
        goto bail;

    numThreads = dataKeys->numThreads;

#if 0
    FILE *dumpStream = fopen("debug", "w");
#endif
    while (1) {
        int threadId;
        unsigned int methodVal;
        int action;
        unsigned int methodId;
        CallStack *pStack;

        /*
         * Extract values from file.
         */
        if (readDataRecord(dataFp, &threadId, &methodVal, &currentTime))
            break;

        action = METHOD_ACTION(methodVal);
        methodId = METHOD_ID(methodVal);

        /* Get the call stack for this thread */
        pStack = traceData->stacks[threadId];

        /* If there is no call stack yet for this thread, then allocate one */
        if (pStack == NULL) {
            pStack = malloc(sizeof(CallStack));
            pStack->top = 0;
            pStack->lastEventTime = currentTime;
            pStack->threadStartTime = currentTime;
	    pStack->remTimes = (uint64_t*) calloc(numFilters, sizeof(uint64_t));
            traceData->stacks[threadId] = pStack;
        }

        /* Lookup the current method */
        method = lookupMethod(dataKeys, methodId);
        if (method == NULL)
            method = &dataKeys->methods[UNKNOWN_INDEX];

#if 0
        if (method->methodName) {
	    fprintf(dumpStream, "%2d %-8llu %d %8llu r %d c %d %s.%s %s\n",
	           threadId, currentTime, action, pStack->threadStartTime,
	           method->recursiveEntries,
	           pStack->top, method->className, method->methodName,
	           method->signature);
        } else {
	    printf(dumpStream, "%2d %-8llu %d %8llu r %d c %d %s\n",
	           threadId, currentTime, action, pStack->threadStartTime,
	           method->recursiveEntries,
	           pStack->top, method->className);
        }
#endif

        if (action == METHOD_TRACE_ENTER) {
            /* This is a method entry */
            if (pStack->top >= MAX_STACK_DEPTH) {
                fprintf(stderr, "Stack overflow (exceeded %d frames)\n",
                        MAX_STACK_DEPTH);
                exit(1);
            }

            /* Get the caller method */
            if (pStack->top >= 1)
                caller = pStack->calls[pStack->top - 1].method;
            else
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            countRecursiveEntries(pStack, pStack->top, caller);
            caller->elapsedExclusive += currentTime - pStack->lastEventTime;
#if 0
            if (caller->elapsedExclusive > 10000000)
                fprintf(dumpStream, "%llu current %llu last %llu diff %llu\n",
                        caller->elapsedExclusive, currentTime,
                        pStack->lastEventTime,
                        currentTime - pStack->lastEventTime);
#endif
            if (caller->recursiveEntries <= 1) {
                caller->topExclusive += currentTime - pStack->lastEventTime;
            }

            /* Push the method on the stack for this thread */
            pStack->calls[pStack->top].method = method;
            pStack->calls[pStack->top++].entryTime = currentTime;

	    // For each filter
	    int result = 0;
	    for (ii = 0; ii < numFilters; ii++) {
	        result = filterMethod(method, filters[ii], 1, threadId, numThreads,
				       currentTime - pStack->lastEventTime, pStack->remTimes[ii]);

		// TODO: make remTimes work properly
		// Consider moving remTimes handling together with the rest
		// of time handling and clean up the return codes
		/*
		if (result == 0) { // no time added, no remTime added
		    pStack->remTimes[ii] += currentTime - pStack->lastEventTime;
		} else if (result == 3 || result == 4) { // remTime added
		    // Reset remTime, since it's been added
		    pStack->remTimes[ii] = 0;
		}
		*/
	    }

        } else {
            /* This is a method exit */
            uint64_t entryTime = 0;

            /* Pop the method off the stack for this thread */
            if (pStack->top > 0) {
                pStack->top -= 1;
                entryTime = pStack->calls[pStack->top].entryTime;
                if (method != pStack->calls[pStack->top].method) {
                    if (method->methodName) {
                        fprintf(stderr,
                            "Exit from method %s.%s %s does not match stack:\n",
                            method->className, method->methodName,
                            method->signature);
                    } else {
                        fprintf(stderr,
                            "Exit from method %s does not match stack:\n",
                            method->className);
                    }
                    stackDump(pStack, pStack->top + 1);
                    exit(1);
                }
            }

            /* Get the caller method */
            if (pStack->top >= 1)
                caller = pStack->calls[pStack->top - 1].method;
            else
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            countRecursiveEntries(pStack, pStack->top, caller);
            countRecursiveEntries(pStack, pStack->top, method);
            uint64_t elapsed = currentTime - entryTime;
            addInclusiveTime(caller, method, elapsed);
            method->elapsedExclusive += currentTime - pStack->lastEventTime;
            if (method->recursiveEntries == 0) {
                method->topExclusive += currentTime - pStack->lastEventTime;
            }

	    // For each filter
	    int result = 0;
	    for (ii = 0; ii < numFilters; ii++) {
	        result = filterMethod(method, filters[ii], 0, threadId, numThreads,
				       currentTime - pStack->lastEventTime, pStack->remTimes[ii]);

		// TODO: make remTimes work properly
		/*
		if (result == 0) { // no time added, no remTime added
		    pStack->remTimes[ii] += currentTime - pStack->lastEventTime;
		} else if (result == 3 || result == 4) { // remTime added
		    // Reset remTime, since it's been added
		    pStack->remTimes[ii] = 0;
		}
		*/
	    }

        }
        /* Remember the time of the last entry or exit event */
        pStack->lastEventTime = currentTime;
    }

    /* If we have calls on the stack when the trace ends, then clean
     * up the stack and add time to the callers by pretending that we
     * are exiting from their methods now.
     */
    CallStack *pStack;
    int threadId;
    uint64_t elapsedTime = 0;
    uint64_t sumThreadTime = 0;
    for (threadId = 0; threadId < MAX_THREADS; ++threadId) {

        pStack = traceData->stacks[threadId];

        /* If this thread never existed, then continue with next thread */
        if (pStack == NULL)
            continue;

        /* Calculate times spent in thread, and add it to total time */
        elapsedTime = pStack->lastEventTime - pStack->threadStartTime;
        sumThreadTime += elapsedTime;

        for (ii = 0; ii < pStack->top; ++ii) {
	  //printf("in loop\n");

            if (ii == 0)
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            else
                caller = pStack->calls[ii - 1].method;
            method = pStack->calls[ii].method;
            countRecursiveEntries(pStack, ii, caller);
            countRecursiveEntries(pStack, ii, method);

            uint64_t entryTime = pStack->calls[ii].entryTime;
            uint64_t elapsed = pStack->lastEventTime - entryTime;
            addInclusiveTime(caller, method, elapsed);

	    // For each filter
	    int result = 0;
	    for (ii = 0; ii < numFilters; ii++) {
	        result = filterMethod(method, filters[ii], 0, threadId, numThreads,
				       currentTime - pStack->lastEventTime, pStack->remTimes[ii]);

		// TODO: make remTimes work properly
		/*
		if (result == 0) { // no time added, no remTime added
		    pStack->remTimes[ii] += currentTime - pStack->lastEventTime;
		} else if (result == 3 || result == 4) { // remTime added
		    // Reset remTime, since it's been added
		    pStack->remTimes[ii] = 0;
		}
		*/
	    }
        }

	/* Save the per-thread elapsed time in the DataKeys struct */
	for (ii = 0; ii < dataKeys->numThreads; ++ii) {
	    if (dataKeys->threads[ii].threadId == threadId) {
                dataKeys->threads[ii].elapsedTime = elapsedTime;
	    }
	}


    }
    caller = &dataKeys->methods[TOPLEVEL_INDEX];
    caller->elapsedInclusive = sumThreadTime;

#if 0
    fclose(dumpStream);
#endif

    if (threadTime != NULL) {
        *threadTime = sumThreadTime;
    }

bail:
    if (dataFp != NULL)
        fclose(dataFp);

    return dataKeys;
}

MethodEntry** parseMethodEntries(DataKeys* dataKeys)
{
    int ii;
    /* Create a new array of pointers to the methods and sort the pointers
     * instead of the actual MethodEntry structs.  We need to do this
     * because there are other lists that contain pointers to the
     * MethodEntry structs.
     */
    MethodEntry** pMethods = (MethodEntry**) malloc(sizeof(MethodEntry*) * dataKeys->numMethods);
    for (ii = 0; ii < dataKeys->numMethods; ++ii) {
        MethodEntry* entry = &dataKeys->methods[ii];
        pMethods[ii] = entry;
    }

    return pMethods;
}


/*
 * Produce a function profile from the following methods
 */
void profileTrace(TraceData* traceData, MethodEntry **pMethods, int numMethods, uint64_t sumThreadTime,
                  ThreadEntry *pThreads, int numThreads, Filter** filters)
{
   /* Print the html header, if necessary */
    if (gOptions.outputHtml) {
        printf(htmlHeader, gOptions.sortableUrl);
        outputTableOfContents();
    }

    printExclusiveProfile(pMethods, numMethods, sumThreadTime);
    printInclusiveProfile(pMethods, numMethods, sumThreadTime);

    printThreadProfile(pThreads, numThreads, sumThreadTime, filters);

    createClassList(traceData, pMethods, numMethods);
    printClassProfiles(traceData, sumThreadTime);

    createUniqueMethodList(traceData, pMethods, numMethods);
    printMethodProfiles(traceData, sumThreadTime);

    if (gOptions.outputHtml) {
        printf("%s", htmlFooter);
    }
}

int compareMethodNamesForDiff(const void *a, const void *b)
{
    int result;

    const MethodEntry *methodA = *(const MethodEntry**)a;
    const MethodEntry *methodB = *(const MethodEntry**)b;
    if (methodA->methodName == NULL || methodB->methodName == NULL) {
        return compareClassNames(a, b);
    }
    result = strcmp(methodA->methodName, methodB->methodName);
    if (result == 0) {
        result = strcmp(methodA->signature, methodB->signature);
        if (result == 0) {
           return strcmp(methodA->className, methodB->className);
        }
    }
    return result;
}

int findMatch(MethodEntry** methods, int size, MethodEntry* matchThis)
{
    int i;

    for (i = 0 ; i < size ; i++) {
        MethodEntry* method = methods[i];

        if (method != NULL && !compareMethodNamesForDiff(&method, &matchThis)) {
//            printf("%s.%s == %s.%s<br>\n", matchThis->className, matchThis->methodName,
  //              method->className, method->methodName);

            return i;
/*            if (!compareMethodNames(&method, &matchThis)) {
                return i;
            }
*/        }
    }

    return -1;
}

int compareDiffEntriesExculsive(const void *a, const void *b)
{
    int result;

    const DiffEntry* entryA = (const DiffEntry*)a;
    const DiffEntry* entryB = (const DiffEntry*)b;

    if (entryA->differenceExclusive < entryB->differenceExclusive) {
        return 1;
    } else if (entryA->differenceExclusive > entryB->differenceExclusive) {
        return -1;
    }

    return 0;
}

int compareDiffEntriesInculsive(const void *a, const void *b)
{
    int result;

    const DiffEntry* entryA = (const DiffEntry*)a;
    const DiffEntry* entryB = (const DiffEntry*)b;

    if (entryA->differenceInclusive < entryB->differenceInclusive) {
        return 1;
    } else if (entryA->differenceInclusive > entryB->differenceInclusive) {
        return -1;
    }

    return 0;
}

void printMissingMethod(MethodEntry* method)
{
    char classBuf[HTML_BUFSIZE];
    char methodBuf[HTML_BUFSIZE];
    char* className;
    char* methodName;

    className = htmlEscape(method->className, classBuf, HTML_BUFSIZE);
    methodName = htmlEscape(method->methodName, methodBuf, HTML_BUFSIZE);

    if (gOptions.outputHtml) printf("<tr><td>\n");

    printf("%s.%s ", className, methodName);
    if (gOptions.outputHtml) printf("</td><td>");

    printf("%lld ", method->elapsedExclusive);
    if (gOptions.outputHtml) printf("</td><td>");

    printf("%lld ", method->elapsedInclusive);
    if (gOptions.outputHtml) printf("</td><td>");

    printf("%d\n", method->numCalls[0]);
    if (gOptions.outputHtml) printf("</td><td>\n");
}


void createDiff(DataKeys* d1, uint64_t sum1, DataKeys* d2, uint64_t sum2)
{
    MethodEntry** methods1 = parseMethodEntries(d1);
    MethodEntry** methods2 = parseMethodEntries(d2);

    // sort and assign the indicies
    int i;
    qsort(methods1, d1->numMethods, sizeof(MethodEntry*), compareElapsedInclusive);
    for (i = 0; i < d1->numMethods; ++i) {
        methods1[i]->index = i;
    }

    qsort(methods2, d2->numMethods, sizeof(MethodEntry*), compareElapsedInclusive);
    for (i = 0; i < d2->numMethods; ++i) {
        methods2[i]->index = i;
    }

    int max = (d1->numMethods < d2->numMethods) ? d2->numMethods : d1->numMethods;
    max++;
    DiffEntry* diffs = (DiffEntry*)malloc(max * sizeof(DiffEntry));
    memset(diffs, 0, max * sizeof(DiffEntry));
    DiffEntry* ptr = diffs;

//    printf("<br>d1->numMethods: %d d1->numMethods: %d<br>\n", d1->numMethods, d2->numMethods);

    int matches = 0;

    for (i = 0 ; i < d1->numMethods ; i++) {
        int match = findMatch(methods2, d2->numMethods, methods1[i]);
        if (match >= 0) {
            ptr->method1 = methods1[i];
            ptr->method2 = methods2[match];

            uint64_t e1 = ptr->method1->elapsedExclusive;
            uint64_t e2 = ptr->method2->elapsedExclusive;
            if (e1 > 0) {
                ptr->differenceExclusive = e2 - e1;
                ptr->differenceExclusivePercentage = ((double)e2 / (double)e1) * 100.0;
            }

            uint64_t i1 = ptr->method1->elapsedInclusive;
            uint64_t i2 = ptr->method2->elapsedInclusive;
            if (i1 > 0) {
                ptr->differenceInclusive = i2 - i1;
                ptr->differenceInclusivePercentage = ((double)i2 / (double)i1) * 100.0;
            }

            // clear these out so we don't find them again and we know which ones
            // we have left over
            methods1[i] = NULL;
            methods2[match] = NULL;
            ptr++;

            matches++;
        }
    }
    ptr->method1 = NULL;
    ptr->method2 = NULL;

    qsort(diffs, matches, sizeof(DiffEntry), compareDiffEntriesExculsive);
    ptr = diffs;

    if (gOptions.outputHtml) {
        printf(htmlHeader, gOptions.sortableUrl);
        printf("<h3>Table of Contents</h3>\n");
        printf("<ul>\n");
        printf("<li><a href='#exclusive'>Exclusive</a>\n");
        printf("<li><a href='#inclusive'>Inclusive</a>\n");
        printf("</ul>\n");
        printf("Run 1: %s<br>\n", gOptions.diffFileName);
        printf("Run 2: %s<br>\n", gOptions.traceFileName);
        printf("<a name=\"exclusive\"></a><h3 id=\"exclusive\">Exclusive</h3>\n");
        printf(tableHeader, "exclusive_table");
    }

    char classBuf[HTML_BUFSIZE];
    char methodBuf[HTML_BUFSIZE];
    char* className;
    char* methodName;

    while (ptr->method1 != NULL && ptr->method2 != NULL) {
        if (gOptions.outputHtml) printf("<tr><td>\n");

        className = htmlEscape(ptr->method1->className, classBuf, HTML_BUFSIZE);
        methodName = htmlEscape(ptr->method1->methodName, methodBuf, HTML_BUFSIZE);

        printf("%s.%s ", className, methodName);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%lld ", ptr->method1->elapsedExclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%llu ", ptr->method2->elapsedExclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%lld ", ptr->differenceExclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%.2f\n", ptr->differenceExclusivePercentage);
        if (gOptions.outputHtml) printf("</td><td>\n");

        printf("%d\n", ptr->method1->numCalls[0]);
        if (gOptions.outputHtml) printf("</td><td>\n");

        printf("%d\n", ptr->method2->numCalls[0]);
        if (gOptions.outputHtml) printf("</td></tr>\n");

        ptr++;
    }

    if (gOptions.outputHtml) printf("</table>\n");

    if (gOptions.outputHtml) {
        printf(htmlHeader, gOptions.sortableUrl);
        printf("Run 1: %s<br>\n", gOptions.diffFileName);
        printf("Run 2: %s<br>\n", gOptions.traceFileName);
        printf("<a name=\"inclusive\"></a><h3 id=\"inculisve\">Inclusive</h3>\n");
        printf(tableHeader, "inclusive_table");
    }

    qsort(diffs, matches, sizeof(DiffEntry), compareDiffEntriesInculsive);
    ptr = diffs;

    while (ptr->method1 != NULL && ptr->method2 != NULL) {
        if (gOptions.outputHtml) printf("<tr><td>\n");

        className = htmlEscape(ptr->method1->className, classBuf, HTML_BUFSIZE);
        methodName = htmlEscape(ptr->method1->methodName, methodBuf, HTML_BUFSIZE);

        printf("%s.%s ", className, methodName);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%lld ", ptr->method1->elapsedInclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%llu ", ptr->method2->elapsedInclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%lld ", ptr->differenceInclusive);
        if (gOptions.outputHtml) printf("</td><td>");

        printf("%.2f\n", ptr->differenceInclusivePercentage);
        if (gOptions.outputHtml) printf("</td><td>\n");

        printf("%d\n", ptr->method1->numCalls[0]);
        if (gOptions.outputHtml) printf("</td><td>\n");

        printf("%d\n", ptr->method2->numCalls[0]);
        if (gOptions.outputHtml) printf("</td></tr>\n");

        ptr++;
    }

    if (gOptions.outputHtml) {
        printf("</table>\n");
        printf("<h3>Run 1 methods not found in Run 2</h3>");
        printf(tableHeaderMissing);
    }

    for (i = 0; i < d1->numMethods; ++i) {
        if (methods1[i] != NULL) {
           printMissingMethod(methods1[i]);
        }
    }

    if (gOptions.outputHtml) {
        printf("</table>\n");
        printf("<h3>Run 2 methods not found in Run 1</h3>");
        printf(tableHeaderMissing);
    }

    for (i = 0; i < d2->numMethods; ++i) {
        if (methods2[i] != NULL) {
            printMissingMethod(methods2[i]);
        }
    }

    if (gOptions.outputHtml) printf("</body></html\n");
}

int usage(const char *program)
{
    fprintf(stderr, "usage: %s [-ho] [-s sortable] [-d trace-file-name] [-g outfile] [-f filter-file] trace-file-name\n", program);
    fprintf(stderr, "  -d trace-file-name  - Diff with this trace\n");
    fprintf(stderr, "  -g outfile          - Write graph to 'outfile'\n");
    fprintf(stderr, "  -f filter-file      - Filter functions as specified in file\n");
    fprintf(stderr, "  -k                  - When writing a graph, keep the intermediate DOT file\n");
    fprintf(stderr, "  -h                  - Turn on HTML output\n");
    fprintf(stderr, "  -o                  - Dump the dmtrace file instead of profiling\n");
    fprintf(stderr, "  -s                  - URL base to where the sortable javascript file\n");
    fprintf(stderr, "  -t threshold        - Threshold percentage for including nodes in the graph\n");
    return 2;
}

// Returns true if there was an error
int parseOptions(int argc, char **argv)
{
    while (1) {
        int opt = getopt(argc, argv, "d:hg:kos:t:f:");
        if (opt == -1)
            break;
        switch (opt) {
            case 'd':
                gOptions.diffFileName = optarg;
                break;
            case 'g':
                gOptions.graphFileName = optarg;
                break;
            case 'f':
	        gOptions.filterFileName = optarg;
                break;
            case 'k':
                gOptions.keepDotFile = 1;
                break;
            case 'h':
                gOptions.outputHtml = 1;
                break;
            case 'o':
                gOptions.dump = 1;
                break;
            case 's':
                gOptions.sortableUrl = optarg;
                break;
            case 't':
                gOptions.threshold = atoi(optarg);
                break;
            default:
                return 1;
        }
    }
    return 0;
}

/*
 * Parse args.
 */
int main(int argc, char** argv)
{

    gOptions.threshold = -1;

    // Parse the options
    if (parseOptions(argc, argv) || argc - optind != 1)
        return usage(argv[0]);

    gOptions.traceFileName = argv[optind];

    if (gOptions.threshold < 0 || 100 <= gOptions.threshold) {
        gOptions.threshold = 20;
    }

    if (gOptions.dump) {
        dumpTrace();
        return 0;
    }

    uint64_t sumThreadTime = 0;

    Filter** filters = NULL;
    if (gOptions.filterFileName != NULL) {
        filters = parseFilters(gOptions.filterFileName);
    }

    TraceData data1;
    memset(&data1, 0, sizeof(data1));
    DataKeys* dataKeys = parseDataKeys(&data1, gOptions.traceFileName,
                                       &sumThreadTime, filters);
    if (dataKeys == NULL) {
        fprintf(stderr, "Cannot read trace.\n");
        exit(1);
    }

    if (gOptions.diffFileName != NULL) {
        uint64_t sum2;
        TraceData data2;
        DataKeys* d2 = parseDataKeys(&data2, gOptions.diffFileName, &sum2, filters);

        createDiff(d2, sum2, dataKeys, sumThreadTime);

        freeDataKeys(d2);
    } else {
        MethodEntry** methods = parseMethodEntries(dataKeys);
        profileTrace(&data1, methods, dataKeys->numMethods, sumThreadTime,
                     dataKeys->threads, dataKeys->numThreads, filters);
        if (gOptions.graphFileName != NULL) {
            createInclusiveProfileGraphNew(dataKeys);
        }
        free(methods);
    }

    freeDataKeys(dataKeys);

    return 0;
}
