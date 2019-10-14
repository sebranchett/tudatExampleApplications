/*    Copyright (c) 2010-2017, Delft University of Technology
*    All rigths reserved
*
*    This file is part of the Tudat. Redistribution and use in source and
*    binary forms, with or without modification, are permitted exclusively
*    under the terms of the Modified BSD license. You should have received
*    a copy of the license with this file. If not, please or visit:
*    http://tudat.tudelft.nl/LICENSE.
*/

#include <iostream>
#include <fstream>

#include <boost/filesystem.hpp>
#include "Problems/applicationOutput.h"
#include "Problems/getAlgorithm.h"
#include "Problems/saveOptimizationResults.h"

#include "Tudat/Astrodynamics/LowThrustDirectMethods/lowThrustOptimisationSetup.h"
#include "Tudat/Astrodynamics/ShapeBasedMethods/hodographicShaping.h"
#include "Tudat/Astrodynamics/ShapeBasedMethods/createBaseFunctionHodographicShaping.h"
#include "Tudat/Astrodynamics/LowThrustDirectMethods/lowThrustLegSettings.h"
#include "Tudat/Astrodynamics/Ephemerides/approximatePlanetPositions.h"
#include "Problems/lowThrustTrajectory.h"
#include "Problems/getRecommendedBaseFunctionsHodographicShaping.h"

//! Execute  main
int main( )
{
    //Set seed for reproducible results
    pagmo::random_device::set_seed( 123 );

    double julianDateAtDeparture = 8174.5 * physical_constants::JULIAN_DAY;
    double  timeOfFlight = 580.0 * physical_constants::JULIAN_DAY;

    // Ephemeris departure body.
    ephemerides::EphemerisPointer pointerToDepartureBodyEphemeris = std::make_shared< ephemerides::ApproximatePlanetPositions>(
                ephemerides::ApproximatePlanetPositionsBase::BodiesWithEphemerisData::earthMoonBarycenter );

    // Ephemeris arrival body.
    ephemerides::EphemerisPointer pointerToArrivalBodyEphemeris = std::make_shared< ephemerides::ApproximatePlanetPositions >(
                ephemerides::ApproximatePlanetPositionsBase::BodiesWithEphemerisData::mars );

    // Retrieve cartesian state at departure and arrival.
    Eigen::Vector6d cartesianStateAtDeparture = pointerToDepartureBodyEphemeris->getCartesianState( julianDateAtDeparture );
    Eigen::Vector6d cartesianStateAtArrival = pointerToArrivalBodyEphemeris->getCartesianState( julianDateAtDeparture + timeOfFlight );

    std::function< Eigen::Vector6d( const double ) > departureStateFunction = [ = ]( const double currentTime )
    {
        return pointerToDepartureBodyEphemeris->getCartesianState( currentTime );
    };

    std::function< Eigen::Vector6d( const double ) > arrivalStateFunction = [ = ]( const double currentTime )
    {
        return pointerToArrivalBodyEphemeris->getCartesianState( currentTime );
    };



    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////         SET UP DYNAMICAL ENVIRONMENT                    /////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    spice_interface::loadStandardSpiceKernels( );

    // Create central, departure and arrival bodies.
    std::vector< std::string > bodiesToCreate;
    bodiesToCreate.push_back( "Sun" );

    std::map< std::string, std::shared_ptr< simulation_setup::BodySettings > > bodySettings =
            simulation_setup::getDefaultBodySettings( bodiesToCreate );

    std::string frameOrigin = "SSB";
    std::string frameOrientation = "ECLIPJ2000";


    // Define central body ephemeris settings.
    bodySettings[ "Sun" ]->ephemerisSettings = std::make_shared< simulation_setup::ConstantEphemerisSettings >(
                ( Eigen::Vector6d( ) << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 ).finished( ), frameOrigin, frameOrientation );

    bodySettings[ "Sun" ]->ephemerisSettings->resetFrameOrientation( frameOrientation );
    bodySettings[ "Sun" ]->rotationModelSettings->resetOriginalFrame( frameOrientation );


    // Create body map.
    simulation_setup::NamedBodyMap bodyMap = createBodies( bodySettings );

    bodyMap[ "Borzi" ] = std::make_shared< simulation_setup::Body >( );
    bodyMap.at( "Borzi" )->setEphemeris( std::make_shared< ephemerides::TabulatedCartesianEphemeris< > >(
                                                         std::shared_ptr< interpolators::OneDimensionalInterpolator
                                                         < double, Eigen::Vector6d > >( ), frameOrigin, frameOrientation ) );


    setGlobalFrameBodyEphemerides( bodyMap, frameOrigin, frameOrientation );


    // Define body to propagate and central body.
    std::vector< std::string > bodiesToPropagate;
    bodiesToPropagate.push_back( "Borzi" );
    std::vector< std::string > centralBodies;
    centralBodies.push_back( "Sun" );

    // Set vehicle mass.
    bodyMap[ "Borzi" ]->setConstantBodyMass( 2000.0 );


    // Define specific impulse function.
    std::function< double( const double ) > specificImpulseFunction = [ = ]( const double currentTime )
    {
        return 3000.0;
    };


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////        GRID SEARCH FOR HODOGRAPHIC SHAPING LOWEST-ORDER SOLUTION            /////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Define bounds for departure date and time-of-flight.
    std::pair< double, double > departureTimeBounds = std::make_pair( 7304.5 * physical_constants::JULIAN_DAY, 10225.5 * physical_constants::JULIAN_DAY  );
    std::pair< double, double > timeOfFlightBounds = std::make_pair( 500.0 * physical_constants::JULIAN_DAY, 2000.0 * physical_constants::JULIAN_DAY );

    // Initialize free coefficients vector for radial velocity function.
    Eigen::VectorXd freeCoefficientsRadialVelocityFunction = Eigen::VectorXd::Zero( 0 );

    // Initialize free coefficients vector for normal velocity function.
    Eigen::VectorXd freeCoefficientsNormalVelocityFunction = Eigen::VectorXd::Zero( 0 );

    // Initialize free coefficients vector for axial velocity function.
    Eigen::VectorXd freeCoefficientsAxialVelocityFunction = Eigen::VectorXd::Zero( 0 );

    std::map< int, Eigen::Vector4d > hodographicShapingResults;

    int numberCases = 0;

    // for-loop parsing the time-of-flight values, ranging from 500 to 2000 days, with a time-step of 5 days.
    for ( int i = 0 ; i <= ( timeOfFlightBounds.second - timeOfFlightBounds.first ) / ( 5.0 * physical_constants::JULIAN_DAY ) ; i++  )
    {
        double currentTOF = timeOfFlightBounds.first + i * 5.0 * physical_constants::JULIAN_DAY;

        // Get recommended base functions for the radial velocity composite function.
        std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > radialVelocityFunctionComponents;
        getRecommendedRadialVelocityBaseFunctions( radialVelocityFunctionComponents, freeCoefficientsRadialVelocityFunction, currentTOF );

        // Get recommended base functions for the normal velocity composite function.
        std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > normalVelocityFunctionComponents;
        getRecommendedNormalAxialBaseFunctions( normalVelocityFunctionComponents, freeCoefficientsNormalVelocityFunction, currentTOF );

        // for-loop parsing the departure date values, ranging from 7304 MJD to 10225 MJD (with 401 steps)
        for ( int j = 0 ; j <= 400; j++ )
        {
            double currentDepartureDate = departureTimeBounds.first + j * ( departureTimeBounds.second - departureTimeBounds.first ) / 400.0;

            // Compute states at departure and arrival.
            cartesianStateAtDeparture = pointerToDepartureBodyEphemeris->getCartesianState( currentDepartureDate );
            cartesianStateAtArrival = pointerToArrivalBodyEphemeris->getCartesianState( currentDepartureDate + currentTOF );


            int bestNumberOfRevolutions;
            double currentBestDeltaV;

            // Parse shaped trajectories with numbers of revolutions between 0 and 5.
            for ( int currentNumberOfRevolutions = 0 ; currentNumberOfRevolutions <= 5 ; currentNumberOfRevolutions++ )
            {

                // Get recommended base functions for the axial velocity composite function.
                std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > axialVelocityFunctionComponents;
                getRecommendedAxialVelocityBaseFunctions( axialVelocityFunctionComponents, freeCoefficientsAxialVelocityFunction, currentTOF,
                                                          currentNumberOfRevolutions );

                // Create hodographically shaped trajectory.
                tudat::shape_based_methods::HodographicShaping hodographicShaping = shape_based_methods::HodographicShaping(
                            cartesianStateAtDeparture, cartesianStateAtArrival, currentTOF, currentNumberOfRevolutions, bodyMap, "Borzi", "Sun",
                            radialVelocityFunctionComponents, normalVelocityFunctionComponents, axialVelocityFunctionComponents,
                            freeCoefficientsRadialVelocityFunction, freeCoefficientsNormalVelocityFunction, freeCoefficientsAxialVelocityFunction );

                // Save trajectory with the lowest deltaV.
                if ( currentNumberOfRevolutions == 0 )
                {
                    bestNumberOfRevolutions = 0;
                    currentBestDeltaV = hodographicShaping.computeDeltaV( );
                }
                else
                {
                    if ( hodographicShaping.computeDeltaV( ) < currentBestDeltaV )
                    {
                        currentBestDeltaV = hodographicShaping.computeDeltaV( );
                        bestNumberOfRevolutions = currentNumberOfRevolutions;
                    }
                }
            }

            // Save results.
            Eigen::Vector4d outputVector = ( Eigen::Vector4d( ) << currentTOF / physical_constants::JULIAN_DAY,
                                             currentDepartureDate / physical_constants::JULIAN_DAY, currentBestDeltaV, bestNumberOfRevolutions ).finished( );
            numberCases++;
            hodographicShapingResults[ numberCases ] = outputVector;

        }
    }

    input_output::writeDataMapToTextFile( hodographicShapingResults,
                                          "hodographicShapingGridSearch.dat",
                                          "C:/tudatBundle/tudatExampleApplications/libraryExamples/PaGMOEx/SimulationOutput/",
                                          "",
                                          std::numeric_limits< double >::digits10,
                                          std::numeric_limits< double >::digits10,
                                          "," );


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////        RESTRICTED GRID SEARCH FOR HODOGRAPHIC SHAPING HIGH-ORDER SOLUTION             ///////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    numberCases = 0;

    // Define lower and upper bounds for the radial velocity free coefficients.
    std::vector< std::vector< double > > bounds( 2, std::vector< double >( 2, 0.0 ) );
    bounds[ 0 ][ 0 ] = - 600.0;
    bounds[ 1 ][ 0 ] = 800.0;
    bounds[ 0 ][ 1 ] = 0.0;
    bounds[ 1 ][ 1 ] = 1500.0;

    // Set fixed number of revolutions.
    int numberOfRevolutions = 1;

    std::map< int, Eigen::Vector4d > hodographicShapingResultsHigherOrder;
    std::map< int, Eigen::Vector4d > hodographicShapingResultsLowResultOneRevolution;

    // for-loop parsing the time-of-flight values, ranging from 500 to 900 days, with a time-step of 20 days.
    for ( int i = 0 ; i <= ( 900.0 * physical_constants::JULIAN_DAY - timeOfFlightBounds.first ) / ( 20 * physical_constants::JULIAN_DAY ) ; i++  )
    {
        double currentTOF = timeOfFlightBounds.first + i * 20.0 * physical_constants::JULIAN_DAY;

        double frequency = 2.0 * mathematical_constants::PI / currentTOF;
        double scaleFactor = 1.0 / currentTOF;

        // Define settings for the two additional base functions for the radial velocity composite function.
        std::shared_ptr< shape_based_methods::BaseFunctionHodographicShapingSettings > fourthRadialVelocityBaseFunctionSettings =
                std::make_shared< shape_based_methods::PowerTimesTrigonometricFunctionHodographicShapingSettings >( 1.0, 0.5 * frequency, scaleFactor );
        std::shared_ptr< shape_based_methods::BaseFunctionHodographicShapingSettings > fifthRadialVelocityBaseFunctionSettings =
                std::make_shared< shape_based_methods::PowerTimesTrigonometricFunctionHodographicShapingSettings >( 1.0, 0.5 * frequency, scaleFactor );

        // Get recommended base functions for the radial velocity composite function, and add two additional base functions
        // (introducing two degrees of freedom in the trajectory design problem).
        std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > radialVelocityFunctionComponents;
        getRecommendedRadialVelocityBaseFunctions( radialVelocityFunctionComponents, freeCoefficientsRadialVelocityFunction, currentTOF );
        radialVelocityFunctionComponents.push_back(
                    createBaseFunctionHodographicShaping( shape_based_methods::scaledPowerSine, fourthRadialVelocityBaseFunctionSettings ) );
        radialVelocityFunctionComponents.push_back(
                    createBaseFunctionHodographicShaping( shape_based_methods::scaledPowerCosine, fifthRadialVelocityBaseFunctionSettings ) );

        // Get recommended base functions for the normal velocity composite function.
        std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > normalVelocityFunctionComponents;
        getRecommendedNormalAxialBaseFunctions( normalVelocityFunctionComponents, freeCoefficientsNormalVelocityFunction, currentTOF );


        // for-loop parsing departure dates ranging from 7304 MJD to 7379 MJD (with a time-step of 15 days).
        for ( int j = 0 ; j <= ( 7379.5 * physical_constants::JULIAN_DAY - departureTimeBounds.first ) / ( 15 * physical_constants::JULIAN_DAY ); j++ )
        {
            double currentDepartureDate = departureTimeBounds.first +
                    j * 15.0 * physical_constants::JULIAN_DAY;

            // Compute states at departure and arrival.
            cartesianStateAtDeparture = pointerToDepartureBodyEphemeris->getCartesianState( currentDepartureDate );
            cartesianStateAtArrival = pointerToArrivalBodyEphemeris->getCartesianState( currentDepartureDate + currentTOF );


            // Get recommended base functions for the axial velocity composite function.
            std::vector< std::shared_ptr< shape_based_methods::BaseFunctionHodographicShaping > > axialVelocityFunctionComponents;
            getRecommendedAxialVelocityBaseFunctions( axialVelocityFunctionComponents, freeCoefficientsAxialVelocityFunction,
                                                      currentTOF, numberOfRevolutions );


            // Create hodographic shaping optimisation problem.
            problem prob{ HodographicShapingOptimisationProblem( cartesianStateAtDeparture, cartesianStateAtArrival, currentTOF, numberOfRevolutions,
                                                                 bodyMap, "Borzi", "Sun", radialVelocityFunctionComponents,
                                                                 normalVelocityFunctionComponents,
                                                                 axialVelocityFunctionComponents, bounds ) };

            // Perform optimisation.
            algorithm algo{ pagmo::sga( ) };

            // Create an island with 1024 individuals
            island isl{ algo, prob, 1024 };

            // Evolve for 100 generations
            for( int i = 0 ; i < 10; i++ )
            {
                isl.evolve( );
                while( isl.status( ) != pagmo::evolve_status::idle &&
                       isl.status( ) != pagmo::evolve_status::idle_error )
                {
                    isl.wait( );
                }
                isl.wait_check( ); // Raises errors

            }

            // Save high-order shaping solution.
            double currentBestDeltaV = isl.get_population( ).champion_f( )[ 0 ];

            Eigen::Vector4d outputVector = ( Eigen::Vector4d( ) << currentTOF / physical_constants::JULIAN_DAY,
                                             currentDepartureDate / physical_constants::JULIAN_DAY, currentBestDeltaV, 1 ).finished( );

            hodographicShapingResultsHigherOrder[ numberCases ] = outputVector;


            // Compute corresponding low-order hodographic shaping solution.
            Eigen::VectorXd freeCoefficientsRadialVelocityFunction = Eigen::VectorXd::Zero( 2 );
            Eigen::VectorXd freeCoefficientsNormalVelocityFunction = Eigen::VectorXd::Zero( 0 );
            Eigen::VectorXd freeCoefficientsAxialVelocityFunction = Eigen::VectorXd::Zero( 0 );

            // Compute low-order hodographically shaped trajectory (number of revolutions set to 1).
            tudat::shape_based_methods::HodographicShaping hodographicShapingLowOrderOneRevolution = shape_based_methods::HodographicShaping(
                        cartesianStateAtDeparture, cartesianStateAtArrival, currentTOF, numberOfRevolutions, bodyMap, "Borzi", "Sun",
                        radialVelocityFunctionComponents, normalVelocityFunctionComponents, axialVelocityFunctionComponents,
                        freeCoefficientsRadialVelocityFunction, freeCoefficientsNormalVelocityFunction, freeCoefficientsAxialVelocityFunction );

            // Save low-order shaping solution.
            outputVector = ( Eigen::Vector4d( ) << currentTOF / physical_constants::JULIAN_DAY,
                          currentDepartureDate / physical_constants::JULIAN_DAY, hodographicShapingLowOrderOneRevolution.computeDeltaV( ), 1 ).finished( );
            hodographicShapingResultsLowResultOneRevolution[ numberCases ] = outputVector;

            numberCases++;

        }
    }

    input_output::writeDataMapToTextFile( hodographicShapingResultsLowResultOneRevolution,
                                          "hodographicShapingOneRevolution.dat",
                                          "C:/tudatBundle/tudatExampleApplications/libraryExamples/PaGMOEx/SimulationOutput/",
                                          "",
                                          std::numeric_limits< double >::digits10,
                                          std::numeric_limits< double >::digits10,
                                          "," );

    input_output::writeDataMapToTextFile( hodographicShapingResultsHigherOrder,
                                          "hodographicShapingResultsHigherOrder.dat",
                                          "C:/tudatBundle/tudatExampleApplications/libraryExamples/PaGMOEx/SimulationOutput/",
                                          "",
                                          std::numeric_limits< double >::digits10,
                                          std::numeric_limits< double >::digits10,
                                          "," );

    return 0;

}
