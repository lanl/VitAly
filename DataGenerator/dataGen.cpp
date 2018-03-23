#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <stdlib.h>
#include <time.h>

#include <mpi.h>

#include "../genericio/GenericIO.h"

using namespace gio;

int main(int argc, char* argv[])
{
	std::string filename(argv[1]);
	//std::stringstream log;

	//
	// MPI Init
	int myRank, numRanks;
	MPI_Init(NULL,NULL);
	MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
	MPI_Comm_rank(MPI_COMM_WORLD, &myRank);
	MPI_Comm Comm = MPI_COMM_WORLD;


	srand (time(NULL) + myRank);
	
	{
		int dims[3]= {2, 2, 2};
		int periods[3] = { 0, 0, 0 };
		int physOrigin[3] = {0, 0, 0};
		int physScale[3] = {256, 256, 256};
		int numParticles = 1000;


		MPI_Cart_create(Comm, 3, dims, periods, 0, &Comm);

		GenericIO newGIO(Comm, filename);
		newGIO.setNumElems(numParticles);

        for (int d = 0; d < 3; ++d)
        {
            newGIO.setPhysOrigin(physOrigin[d], d);
            newGIO.setPhysScale(physScale[d], d);
        }


		//
		// Variables
		std::vector<float> xx, yy, zz, vx, vy, vz, phi;
		std::vector<uint16_t> mask;

		xx.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		yy.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		zz.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		vx.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		vy.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		vz.resize(numParticles   + newGIO.requestedExtraSpace() / sizeof(float));
		phi.resize(numParticles  + newGIO.requestedExtraSpace() / sizeof(float));
		mask.resize(numParticles + newGIO.requestedExtraSpace() / sizeof(uint16_t));


		

		int offsetX, offsetY, offsetZ;
		if (myRank == 0) //
		{
			offsetX = 0;	offsetY = 0;	offsetZ = 0;
		}
		else if (myRank == 1)
		{
			offsetX = 0;	offsetY = 0;	offsetZ = 128;
		}
		else if (myRank == 2)
		{
			offsetX = 0;	offsetY = 128;	offsetZ = 0;
		}
		else if (myRank == 3)
		{
			offsetX = 0;	offsetY = 128;	offsetZ = 128;
		}
		else if (myRank == 4) //
		{
			offsetX = 128;	offsetY = 0;	offsetZ = 0;
		}
		else if (myRank == 5)
		{
			offsetX = 128;	offsetY = 0;	offsetZ = 128;
		}
		else if (myRank == 6)
		{
			offsetX = 128;	offsetY = 128;	offsetZ = 0;
		}
		else if (myRank == 7)
		{
			offsetX = 128;	offsetY = 128;	offsetZ = 128;
		}

		MPI_Barrier(MPI_COMM_WORLD);
		//log << offsetX << ", " << offsetY << ", " << offsetZ << std::endl;


		double randomNum;
		for (uint64_t i=0; i<numParticles; i++)
		{
			randomNum = ((rand() % 12800)/100.0) + offsetX;	//log << randomNum << ", ";
			xx[i] = randomNum;	

			randomNum = ((rand() % 12800)/100.0) + offsetY;	//log << randomNum << ", ";
			yy[i] = randomNum;

			randomNum = ((rand() % 12800)/100.0) + offsetZ;	//log << randomNum << "\n";
			zz[i] = randomNum;

			vx[i] = (double) (rand() % 1000 / 1000.0);
			vy[i] = (double) (rand() % 1000 / 1000.0);
			vz[i] = (double) (rand() % 1000 / 1000.0);
			phi[i] = (double) (rand() % 1000 / 100.0);
			mask[i] = (uint16_t)myRank;
		}

		int CoordFlagsX = GenericIO::VarIsPhysCoordX;
        int CoordFlagsY = GenericIO::VarIsPhysCoordY;
        int CoordFlagsZ = GenericIO::VarIsPhysCoordZ;

		newGIO.addVariable("x", xx, CoordFlagsX | GenericIO::VarHasExtraSpace);
        newGIO.addVariable("y", yy, CoordFlagsY | GenericIO::VarHasExtraSpace);
		newGIO.addVariable("z", zz, CoordFlagsZ | GenericIO::VarHasExtraSpace);
		newGIO.addVariable("vx", vx, GenericIO::VarHasExtraSpace);
        newGIO.addVariable("vy", vy, GenericIO::VarHasExtraSpace);
        newGIO.addVariable("vz", vz, GenericIO::VarHasExtraSpace);
        newGIO.addVariable("phi", phi, GenericIO::VarHasExtraSpace);
		newGIO.addVariable("mask", mask, GenericIO::VarHasExtraSpace);

        newGIO.write();
	}


	//std::ofstream outputFile( ("dataGEn_" + std::to_string(myRank) + "_.log").c_str(), std::ios::out);
	//outputFile << log.str();
	//outputFile.close();

	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Finalize();

	return 0;
}

// mpirun -np 8 ./dataGen outputFile