/** @file app.h
 * @brief Top-level application header.
 *
 * Umbrella include for the application API. Historically this header carried
 * its own (stale) copy of the application types under the same include guard
 * as <cwist/sys/app/app.h>, which made the two headers mutually exclusive and
 * produced divergent cwist_app layouts across translation units. It now
 * forwards to the canonical header; include either path and you get the same
 * definitions.
 */
#ifndef __CWIST_APP_UMBRELLA_H__
#define __CWIST_APP_UMBRELLA_H__

#include <cwist/sys/app/app.h>

#endif
