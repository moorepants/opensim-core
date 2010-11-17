// testMomentArms.cpp
// Author:  Ajay Seth
/*
* Copyright (c) 2005-2010, Stanford University. All rights reserved. 
* Use of the OpenSim software in source form is permitted provided that the following
* conditions are met:
* 	1. The software is used only for non-commercial research and education. It may not
*     be used in relation to any commercial activity.
* 	2. The software is not distributed or redistributed.  Software distribution is allowed 
*     only through https://simtk.org/home/opensim.
* 	3. Use of the OpenSim software or derivatives must be acknowledged in all publications,
*      presentations, or documents describing work in which OpenSim or derivatives are used.
* 	4. Credits to developers may not be removed from executables
*     created from modifications of the source.
* 	5. Modifications of source code must retain the above copyright notice, this list of
*     conditions and the following disclaimer. 
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
*  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
*  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
*  SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
*  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; 
*  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*  OR BUSINESS INTERRUPTION) OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
*  WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//==========================================================================================================
//	testMomentArms builds various OpenSim models using the OpenSim API and compares moment arm
//  results from these models to the definition r*f = Tau , where r is the moment-arm about a coordinate,
//  f is the scaler maginitude of the Force and Tau is the resulting generalized force. 
//
//	Tests Include:
//      1. ECU muscle from Tutorial 2
//		2. Vasti from gait23 models with and without a patella
//		
//     Add more test cases to address specific problems with moment-arms
//
//==========================================================================================================
#include <OpenSim/OpenSim.h>
#include <iostream>
#include <OpenSim/Common/IO.h>
#include <OpenSim/Simulation/MomentArmSolver.h>
#include "SimTKmath.h"

using namespace OpenSim;
using namespace SimTK;
using namespace std;

#define ASSERT(cond) {if (!(cond)) throw(exception());}
#define ASSERT_EQUAL(expected, found, tolerance) {double tol = std::max((tolerance), std::abs(double (expected)*(tolerance))); if ((found)<(expected)-(tol) || (found)>(expected)+(tol)) throw(exception());}

//==========================================================================================================
// Common Parameters for the simulations are just global.
const static double integ_accuracy = 1.0e-3;
const static double duration = 1.2;
const static Vec3 gravity_vec = Vec3(0, -9.8065, 0);


//==========================================================================================================
// moment_arm = dl/dtheta, definition using inexact peturbation technique
//==========================================================================================================
double computeMomentArmFromDefinition(const State &s, const GeometryPath &path, const Coordinate &coord)
{
	//Compute r = dl/dtheta
	SimTK::State s_ma = s;
	coord.setClamped(s_ma, false);
	coord.setLocked(s_ma, false);
	double theta = coord.getValue(s);
	double dtheta = integ_accuracy;
	
	// Compute length 1 
	coord.setValue(s_ma, theta-dtheta, true);
	double theta1 = coord.getValue(s_ma);
	coord.getModel().getMultibodySystem().realize(s_ma, SimTK::Stage::Position);

	double len1 = path.getLength(s_ma);

	// Compute length 2
	coord.setValue(s_ma, theta+dtheta, true);
	double theta2 = coord.getValue(s_ma);
	coord.getModel().getMultibodySystem().realize(s_ma, SimTK::Stage::Position);

	double len2 = path.getLength(s_ma);

	return (len1-len2)/(theta2-theta1);
}


Vector computeGenForceScaling(const Model &osimModel, const State &s, const Coordinate &coord, 
							  const Array<string> &coupledCoords)
{
	//Local modifiable copy of the state
	State s_ma = s;

	osimModel.getMultibodySystem().realize(s_ma, SimTK::Stage::Instance);

	// Calculate coupling matrix C to determine the influence of other coordinates 
	// (mobilities) on the coordinate of interest due to constraints
    // First declare dummies for the call to project().
    const Vector yWeights(s_ma.getNY(), 1); 
    const Vector cWeights(s_ma.getNMultipliers(), 1);
    Vector yErrEst;

    s_ma.updU() = 0;
	// Light-up speed of coordinate of interest and see how other coordinates
	// affected by constraints respond
    coord.setSpeedValue(s_ma, 1);

	osimModel.getMultibodySystem().realize(s_ma, SimTK::Stage::Velocity);

    osimModel.getMultibodySystem().project(s_ma, 1e-10, 
        yWeights, cWeights, yErrEst, System::ProjectOptions::VelocityOnly);
	
	// Now calculate C. by checking how speeds of other coordinates change
	// normalized by how much the speed of the coordinate of interest changed 
    const Vector C = s_ma.getU() / coord.getSpeedValue(s_ma); 
	
	// Compute the scaling matrix for converting gen_forces to torques
	// Unlike C, ignore all coupling that are not explicit coordinate
	// coupling that defines theta = sum(q_i) or q_i = w_i*theta
	// Also do not consider coupled torques for coordinates not spanned by 
	// the path of interest
	Vector W(osimModel.getNumSpeeds(), 0.0);

	for(int i=0; i< osimModel.getCoordinateSet().getSize(); i++){
		Coordinate &ac = osimModel.getCoordinateSet()[i];
		//If a coordinate is kinematically coupled  (ac.getName() == coord.getName()) || 
		bool found = ((ac.getName() == coord.getName()) || (coupledCoords.findIndex(ac.getName()) > -1));

		// and not translational (cannot contribute to torque)
		if(found && (ac.getMotionType() != Coordinate::Translational) 
				&& (ac.getJoint().getName() != "tib_pat_r") ){
			MobilizedBodyIndex modbodIndex = ac.getBodyIndex();
			const MobilizedBody& mobod = osimModel.getMatterSubsystem().getMobilizedBody(modbodIndex);
			SpatialVec Hcol = mobod.getHCol(s, SimTK::UIndex(0)); //ac.getMobilityIndex())); // get n�th column of H

			double thetaScale = Hcol[0].norm(); // magnitude of the rotational part of this column of H
			
			double Ci = C[mobod.getFirstUIndex(s)+ac.getMobilityIndex()];
			double Wi = 1.0/thetaScale;
			//if(thetaScale)
				W[mobod.getFirstUIndex(s)+ac.getMobilityIndex()] = Ci; 
		}
	}

	return W;
}

//==========================================================================================================
// Main test driver can be used on any model so test cases should be very easy to add
//==========================================================================================================
int testMomentArmDefinitionForModel(const string &filename, const string &coordName ="", 
									const string &muscleName = "", Vec2 rom = Vec2(-Pi/2,0),
									double mass = -1.0)
{
	bool passesDefinition = true;
	bool passesDynamicConsistency = true;

	// Load OpenSim model
	Model osimModel(filename);

	MomentArmSolver maSolver(osimModel);

	Coordinate &coord = (coordName != "") ? osimModel.updCoordinateSet().get(coordName) :
		osimModel.updCoordinateSet()[0];

	// Consider one force, which is the muscle of interest
	Muscle &muscle = (muscleName != "") ? dynamic_cast<Muscle&>((osimModel.updMuscles().get(muscleName))) :
		dynamic_cast<Muscle&>(osimModel.updMuscles()[0]);

	if( mass >= 0.0){
		for(int i=0; i<osimModel.updBodySet().getSize(); i++){
			osimModel.updBodySet()[i].setMass(mass);
			Inertia inertia(mass);
			osimModel.updBodySet()[i].setInertia(inertia);
		}
	}

	SimTK::State &s = osimModel.initSystem();

	Array<string> coupledCoordNames;
	for(int i=0; i<osimModel.getConstraintSet().getSize(); i++){
		OpenSim::Constraint& aConstraint = osimModel.getConstraintSet().get(i);
		if(aConstraint.getType() == "CoordinateCouplerConstraint"){
			CoordinateCouplerConstraint& coupler = dynamic_cast<CoordinateCouplerConstraint&>(aConstraint);
			Array<string> coordNames = coupler.getIndependentCoordinateNames();
			coordNames.append(coupler.getDependentCoordinateName());

			int ind = coordNames.findIndex(coord.getName());
			if (ind > -1){
				for(int j=0; j<coordNames.getSize(); j++){
					if(j!=ind)
						coupledCoordNames.append(coordNames[j]);
				}
			}
		}
	}

	// Reset all speeds to zero
	s.updU() = 0;

	// Disable all forces
	for(int i=0; i<osimModel.updForceSet().getSize(); i++){
		osimModel.updForceSet()[i].setDisabled(s, true);
	}
	// Also disable gravity
	osimModel.getGravityForce().disable(s);

	// Enable just muscle we are interested in.
	muscle.setDisabled(s, false);

	coord.setClamped(s, false);
	coord.setLocked(s, false);

	double q = rom[0];
	int nsteps = 10;
	double dq = (rom[1]-rom[0])/nsteps;
	
	for(int i = 0; i <=nsteps; i++){
		coord.setValue(s, q, true);
		double angle = coord.getValue(s);

		muscle.setActivation(s, 0.1);
		muscle.equilibrate(s);

		//cout << "muscle  force: " << muscle.getForce(s) << endl;
		double ma = muscle.computeMomentArm(s, coord);
		double ma_dldtheta = computeMomentArmFromDefinition(s, muscle.getGeometryPath(), coord);

		cout << "r's = " << ma << "::" << ma_dldtheta <<"  at q = " << coord.getValue(s)*180/Pi; 

		try{
			// Verify that the definition of the moment-arm is satisfied
			ASSERT_EQUAL(ma, ma_dldtheta, integ_accuracy);
		}
		catch(...){
			cout << endl;
			passesDefinition = false;
		}

		// Verify that the moment-arm calculated is dynamically consistent with moment generated
		if(mass!=0 ){
			osimModel.getMultibodySystem().realize(s, Stage::Acceleration);

			double force = muscle.getTendonForce(s);
		
			// Get all applied body forces like those from conact
			const Vector_<SpatialVec>& appliedBodyForces = osimModel.getMultibodySystem().getRigidBodyForces(s, Stage::Dynamics);

			//appliedBodyForces.dump("Applied Body Force resulting from ECU muscle");

			// Get current system accelerations
			const Vector &knownUDots = s.getUDot();
			//knownUDots.dump("Acceleration due to ECU muscle:");

			//Results from an inverse dynamics for the generalized forces to satisfy accelerations
			Vector equivalentGenForce, equivalentGenForceMUdot;

			// Convert body forces to equivalent mobility forces (joint torques)
			osimModel.getMultibodySystem().getMatterSubsystem().calcTreeEquivalentMobilityForces(s, 
				appliedBodyForces, equivalentGenForce);

			if(s.getSystemStage() < SimTK::Stage::Dynamics)
				osimModel.getMultibodySystem().realize(s,SimTK::Stage::Dynamics);

			// Determine the contribution of constraints (if any) to the effective torque
			Vector_<SimTK::SpatialVec> constraintForcesInParent;
			Vector constraintMobilityForces;

			// Get all forces applied to model by constraints
			osimModel.getMultibodySystem().getMatterSubsystem().calcConstraintForcesFromMultipliers(s, s.getMultipliers(), 
				constraintForcesInParent, constraintMobilityForces);
		
			// Perform inverse dynamics
			Vector ivdGenForces;
			osimModel.getMultibodySystem().getMatterSubsystem().calcResidualForceIgnoringConstraints(s,
				0.0*equivalentGenForce, 0.0*appliedBodyForces, knownUDots, ivdGenForces);
			
			//constraintForcesInParent.dump("Constraint Body Forces");
			//constraintMobilityForces.dump("Constraint Mobility Forces");

			Vector W = computeGenForceScaling(osimModel, s, coord, coupledCoordNames);

			double equivalentMuscleTorque = ~W*equivalentGenForce;
			double equivalentIvdMuscleTorque = ~W*(ivdGenForces+constraintMobilityForces);

			cout << "  Tau = " << equivalentIvdMuscleTorque <<"::" << equivalentMuscleTorque 
				 << "  r*fm = " << ma*force <<"::" << ma_dldtheta*force << endl;


			try{	
				// Resulting torque from ID (no constraints) + constraints = equivalent applied torque 
				ASSERT_EQUAL(equivalentIvdMuscleTorque, equivalentMuscleTorque, integ_accuracy);
				// verify that equivalent torque is in fact moment-arm*force
				ASSERT_EQUAL(equivalentIvdMuscleTorque, ma*force, integ_accuracy);
			}
			catch(...){
				passesDynamicConsistency = false;
			}
		}else{
			cout << endl;
		}

		// Increment the joint angle
		q += dq;
	}

	if(!passesDefinition)
		cout << "WARNING: Moment arm did not satisfy dL/dTheta equivalence." << endl;
	if(!passesDynamicConsistency)
		cout << "WARNING: Moment arm * force did not satisfy Torque equivalence." << endl;

	// Minimum requirement to pass is that calculated moment-arm satifies either
	// dL/dTheta definition or is at least dynamically consistent, in which dL/dTheta is not
	int err = (passesDefinition || passesDynamicConsistency) ? 0 : 1;
	return err;
}




int main()
{
	int stat =0, err = 0;

	//err = testMomentArmDefinitionForModel("BothLegs22.osim", "r_knee_angle", "VASINT", Vec2(-2*Pi/3, Pi/18), 0.0);
	//cout << "VASINT of BothLegs with no mass: "  << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	//stat += err;

	err = testMomentArmDefinitionForModel("gait23_PatellaInFemur.osim", "hip_flexion_r", "rect_fem_r", Vec2(-Pi/3, Pi/3));
	cout << "Rectus Femoris at hip with muscle attachment on patella defined w.r.t Femur: "  << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("gait23_PatellaInFemur.osim", "knee_angle_r", "rect_fem_r", Vec2(-2*Pi/3, Pi/18));
	cout << "Rectus Femoris with muscle attachment on patella defined w.r.t Femur: "  << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("gait23_PatellaInFemur.osim", "knee_angle_r", "vas_int_r", Vec2(-2*Pi/3, Pi/18));
	cout << "Knee with Vasti attachment on patella defined w.r.t Femur: "  << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("gait2354_patellae.osim", "knee_angle_r", "vas_int_r", Vec2(-2*Pi/3, Pi/18));
	cout << "Knee with Vasti attachment on patella w.r.t Tibia: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("gait2354_simbody.osim", "knee_angle_r", "vas_int_r", Vec2(-2*Pi/3, Pi/18));
	cout << "Knee with moving muscle point (no patella): " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	//massless should not break moment-arm solver
	cout << ""  << endl;
	err = testMomentArmDefinitionForModel("wrist_mass.osim", "flexion", "ECU_post-surgery", Vec2(-Pi/3, Pi/3), 0.0);
	cout << "WRIST ECU TEST with MASSLESS BODIES: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("wrist_mass.osim", "flexion", "ECU_post-surgery", Vec2(-Pi/3, Pi/3), 1.0);
	cout << "WRIST ECU TEST with MASS  = 1.0 : " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("wrist_mass.osim", "flexion", "ECU_post-surgery", Vec2(-Pi/3, Pi/3), 100.0);
	cout << "WRIST ECU TEST with MASS  = 100.0 : " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("P2PBallJointMomentArmTest.osim");
	cout << "Point to point muscle across BallJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;

	err = testMomentArmDefinitionForModel("P2PBallCustomJointMomentArmTest.osim");
	cout << "Point to point muscle across a ball implemented by CustomJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("MovingPathPointMomentArmTest.osim");
	cout << "Moving path point across PinJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("P2PCustomJointMomentArmTest.osim");
	cout << "Point to point muscle across CustomJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("MovingPointCustomJointMomentArmTest.osim");
	cout << "Moving path point across CustomJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	err = testMomentArmDefinitionForModel("WrapPathCustomJointMomentArmTest.osim");
	cout << "Path with wrapping across CustomJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;
	
	err = testMomentArmDefinitionForModel("PathOnConstrainedBodyMomentArmTest.osim");
	cout << "Path on constrained body across CustomJoint: " << (err ? "FAILED" : "PASSED")  << "\n" << endl;
	stat += err;

	return stat;
}