#ifndef LINCUPL_BANNER_H
#define LINCUPL_BANNER_H

/* Name, version and copyright that every lincupl program prints in its
 * banner and writes into the headers of its output files.  LINCUPL_VERSION
 * is the project version from CMakeLists.txt, passed on the compiler
 * command line. */

#ifndef LINCUPL_VERSION
#error "LINCUPL_VERSION is not defined (it comes from project(VERSION) in CMakeLists.txt)"
#endif

#define PROGRAM_NAME      "LinCUPL"
#define PROGRAM_VERSION   LINCUPL_VERSION
#define PROGRAM_COPYRIGHT "Copyright (c) 2026 LinCUPL team"

#endif /* LINCUPL_BANNER_H */
