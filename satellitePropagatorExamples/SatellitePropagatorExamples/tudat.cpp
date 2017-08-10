/*    Copyright (c) 2010-2017, Delft University of Technology
 *    All rigths reserved
 *
 *    This file is part of the Tudat. Redistribution and use in source and
 *    binary forms, with or without modification, are permitted exclusively
 *    under the terms of the Modified BSD license. You should have received
 *    a copy of the license with this file. If not, please or visit:
 *    http://tudat.tudelft.nl/LICENSE.
 */

#include <getopt.h>

#include <Tudat/External/JsonInterface/simulation.h>

void printHelp( )
{
    std::cout <<
                 "Usage:\n"
                 "\n"
                 "tudat [options] [path]\n"
                 "\n"
                 "path: absolute or relative path to a JSON input file or directory containing a main.json file. "
                 "If not provided, a main.json file will be looked for in the current directory.\n"
                 "\n"
                 "Options:\n"
                 "-h, --help       Show help\n"
              << std::endl;
    exit( EXIT_FAILURE );
}

//! Execute propagation of orbit of Asterix around the Earth.
int main( int argumentCount, char* arguments[ ] )
{
    int currentOption;
    int optionCount = 0;
    const char* const shortOptions = "h";
    const option longOptions[ ] =
    {
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ( ( currentOption = getopt_long( argumentCount, arguments, shortOptions, longOptions, NULL ) ) != -1 )
    {
        switch ( currentOption )
        {
        case 'h':
        case '?':
        default:
            printHelp( );
        }
        optionCount++;
    }

    const int nonOptionArgumentCount = argumentCount - optionCount - 1;
    if ( nonOptionArgumentCount > 1 )
    {
        printHelp( );
    }
    const std::string inputPath = nonOptionArgumentCount == 1 ? arguments[ argumentCount - 1 ] : "";

    tudat::json_interface::Simulation< > simulation( inputPath );
    simulation.run( );
    simulation.exportResults( );

    return EXIT_SUCCESS;
}
