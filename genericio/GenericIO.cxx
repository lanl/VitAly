/*
 *                    Copyright (C) 2015, UChicago Argonne, LLC
 *                               All Rights Reserved
 *
 *                               Generic IO (ANL-15-066)
 *                     Hal Finkel, Argonne National Laboratory
 *
 *                              OPEN SOURCE LICENSE
 *
 * Under the terms of Contract No. DE-AC02-06CH11357 with UChicago Argonne,
 * LLC, the U.S. Government retains certain rights in this software.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the names of UChicago Argonne, LLC or the Department of Energy
 *      nor the names of its contributors may be used to endorse or promote
 *      products derived from this software without specific prior written
 *      permission.
 *
 * *****************************************************************************
 *
 *                                  DISCLAIMER
 * THE SOFTWARE IS SUPPLIED “AS IS” WITHOUT WARRANTY OF ANY KIND.  NEITHER THE
 * UNTED STATES GOVERNMENT, NOR THE UNITED STATES DEPARTMENT OF ENERGY, NOR
 * UCHICAGO ARGONNE, LLC, NOR ANY OF THEIR EMPLOYEES, MAKES ANY WARRANTY,
 * EXPRESS OR IMPLIED, OR ASSUMES ANY LEGAL LIABILITY OR RESPONSIBILITY FOR THE
 * ACCURACY, COMPLETENESS, OR USEFULNESS OF ANY INFORMATION, DATA, APPARATUS,
 * PRODUCT, OR PROCESS DISCLOSED, OR REPRESENTS THAT ITS USE WOULD NOT INFRINGE
 * PRIVATELY OWNED RIGHTS.
 *
 * *****************************************************************************
 */

#define _XOPEN_SOURCE 600
#include "CRC64.h"
#include "GenericIO.h"

extern "C" {
#include "blosc.h"
}

#include <sstream>
#include <set>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

#ifndef GENERICIO_NO_MPI
    #include <ctime>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#ifdef __bgq__
    #include <mpix.h>
#endif

#ifndef MPI_UINT64_T
    #define MPI_UINT64_T (sizeof(long) == 8 ? MPI_LONG : MPI_LONG_LONG)
#endif
using namespace std;


namespace gio {


#ifndef GENERICIO_NO_MPI
GenericFileIO_MPI::~GenericFileIO_MPI() {
    (void) MPI_File_close(&FH);
}

void GenericFileIO_MPI::open(const std::string &FN, bool ForReading) {
    FileName = FN;

    int amode = ForReading ? MPI_MODE_RDONLY : (MPI_MODE_WRONLY | MPI_MODE_CREATE);
    if (MPI_File_open(Comm, const_cast<char *>(FileName.c_str()), amode,
                      MPI_INFO_NULL, &FH) != MPI_SUCCESS)
        throw runtime_error((!ForReading ? "Unable to create the file: " :
                                       "Unable to open the file: ") +
                            FileName);
}

void GenericFileIO_MPI::setSize(size_t sz) {
    if (MPI_File_set_size(FH, sz) != MPI_SUCCESS)
        throw runtime_error("Unable to set size for file: " + FileName);
}

void GenericFileIO_MPI::read(void *buf, size_t count, off_t offset,
                             const std::string &D) {
  while (count > 0) {
        MPI_Status status;
        if (MPI_File_read_at(FH, offset, buf, count, MPI_BYTE, &status) != MPI_SUCCESS)
            throw runtime_error("Unable to read " + D + " from file: " + FileName);

        int scount;
        (void) MPI_Get_count(&status, MPI_BYTE, &scount);

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;
    }
}

void GenericFileIO_MPI::write(const void *buf, size_t count, off_t offset,
                              const std::string &D) {
  while (count > 0) {
        MPI_Status status;
        if (MPI_File_write_at(FH, offset, (void *) buf, count, MPI_BYTE, &status) != MPI_SUCCESS)
            throw runtime_error("Unable to write " + D + " to file: " + FileName);

        int scount;
        (void) MPI_Get_count(&status, MPI_BYTE, &scount);

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;
    }
}

void GenericFileIO_MPICollective::read(void *buf, size_t count, off_t offset,
                             const std::string &D) {
    int Continue = 0;

  do {
        MPI_Status status;
        if (MPI_File_read_at_all(FH, offset, buf, count, MPI_BYTE, &status) != MPI_SUCCESS)
            throw runtime_error("Unable to read " + D + " from file: " + FileName);

    int scount = 0;
    // On some systems, MPI_Get_count will not return zero even when count is zero.
    if (count > 0)
        (void) MPI_Get_count(&status, MPI_BYTE, &scount);

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;

        int NeedContinue = (count > 0);
        MPI_Allreduce(&NeedContinue, &Continue, 1, MPI_INT, MPI_SUM, Comm);
  } while (Continue);
}

void GenericFileIO_MPICollective::write(const void *buf, size_t count, off_t offset,
                              const std::string &D) {
    int Continue = 0;

  do {
        MPI_Status status;
        if (MPI_File_write_at_all(FH, offset, (void *) buf, count, MPI_BYTE, &status) != MPI_SUCCESS)
            throw runtime_error("Unable to write " + D + " to file: " + FileName);

    int scount = 0;
    // On some systems, MPI_Get_count will not return zero even when count is zero.
    if (count > 0)
        (void) MPI_Get_count(&status, MPI_BYTE, &scount);

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;

        int NeedContinue = (count > 0);
        MPI_Allreduce(&NeedContinue, &Continue, 1, MPI_INT, MPI_SUM, Comm);
  } while (Continue);
}
#endif

GenericFileIO_POSIX::~GenericFileIO_POSIX() {
    if (FH != -1) close(FH);
}

void GenericFileIO_POSIX::open(const std::string &FN, bool ForReading) {
    FileName = FN;

    int flags = ForReading ? O_RDONLY : (O_WRONLY | O_CREAT);
    int mode = S_IRUSR | S_IWUSR | S_IRGRP;
    errno = 0;
    if ((FH = ::open(FileName.c_str(), flags, mode)) == -1)
        throw runtime_error((!ForReading ? "Unable to create the file: " :
                                       "Unable to open the file: ") +
                            FileName + ": " + strerror(errno));
}

void GenericFileIO_POSIX::setSize(size_t sz) {
    if (ftruncate(FH, sz) == -1)
        throw runtime_error("Unable to set size for file: " + FileName);
}

void GenericFileIO_POSIX::read(void *buf, size_t count, off_t offset,
                               const std::string &D) {
  while (count > 0) {
        ssize_t scount;
        errno = 0;
    if ((scount = pread(FH, buf, count, offset)) == -1) {
            if (errno == EINTR)
                continue;

      throw runtime_error("Unable to read " + D + " from file: " + FileName +
                                ": " + strerror(errno));
        }

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;
    }
}

void GenericFileIO_POSIX::write(const void *buf, size_t count, off_t offset,
                                const std::string &D) {
  while (count > 0) {
        ssize_t scount;
        errno = 0;
    if ((scount = pwrite(FH, buf, count, offset)) == -1) {
            if (errno == EINTR)
                continue;

            throw runtime_error("Unable to write " + D + " to file: " + FileName +
                                ": " + strerror(errno));
        }

        count -= scount;
        buf = ((char *) buf) + scount;
        offset += scount;
    }
}

static bool isBigEndian() {
    const uint32_t one = 1;
    return !(*((char *)(&one)));
}

static void bswap(void *v, size_t s) {
    char *p = (char *) v;
    for (size_t i = 0; i < s / 2; ++i)
        std::swap(p[i], p[s - (i + 1)]);
}

// Using #pragma pack here, instead of __attribute__((packed)) because xlc, at
// least as of v12.1, won't take __attribute__((packed)) on non-POD and/or
// templated types.
#pragma pack(1)

template <typename T, bool IsBigEndian>
struct endian_specific_value {
  operator T() const {
        T rvalue = value;
        if (IsBigEndian != isBigEndian())
            bswap(&rvalue, sizeof(T));

        return rvalue;
    };

  endian_specific_value &operator = (T nvalue) {
        if (IsBigEndian != isBigEndian())
            bswap(&nvalue, sizeof(T));

        value = nvalue;
        return *this;
    }

  endian_specific_value &operator += (T nvalue) {
        *this = *this + nvalue;
        return *this;
    }

  endian_specific_value &operator -= (T nvalue) {
        *this = *this - nvalue;
        return *this;
    }

  private:
    T value;
};

static const size_t CRCSize = 8;

static const size_t MagicSize = 8;
static const char *MagicBE = "HACC01B";
static const char *MagicLE = "HACC01L";

template <bool IsBigEndian>
struct GlobalHeader {
    char Magic[MagicSize];
    endian_specific_value<uint64_t, IsBigEndian> HeaderSize;
    endian_specific_value<uint64_t, IsBigEndian> NElems; // The global total
    endian_specific_value<uint64_t, IsBigEndian> Dims[3];
    endian_specific_value<uint64_t, IsBigEndian> NVars;
    endian_specific_value<uint64_t, IsBigEndian> VarsSize;
    endian_specific_value<uint64_t, IsBigEndian> VarsStart;
    endian_specific_value<uint64_t, IsBigEndian> NRanks;
    endian_specific_value<uint64_t, IsBigEndian> RanksSize;
    endian_specific_value<uint64_t, IsBigEndian> RanksStart;
    endian_specific_value<uint64_t, IsBigEndian> GlobalHeaderSize;
    endian_specific_value<double,   IsBigEndian> PhysOrigin[3];
    endian_specific_value<double,   IsBigEndian> PhysScale[3];
    endian_specific_value<uint64_t, IsBigEndian> BlocksSize;
    endian_specific_value<uint64_t, IsBigEndian> BlocksStart;
    endian_specific_value<uint64_t, IsBigEndian> OctreeSize;
    endian_specific_value<uint64_t, IsBigEndian> OctreeStart;
};

enum {
    FloatValue          = (1 << 0),
    SignedValue         = (1 << 1),
    ValueIsPhysCoordX   = (1 << 2),
    ValueIsPhysCoordY   = (1 << 3),
    ValueIsPhysCoordZ   = (1 << 4),
    ValueMaybePhysGhost = (1 << 5)
};

static const size_t NameSize = 256;
template <bool IsBigEndian>
struct VariableHeader {
    char Name[NameSize];
    endian_specific_value<uint64_t, IsBigEndian> Flags;
    endian_specific_value<uint64_t, IsBigEndian> Size;
    endian_specific_value<uint64_t, IsBigEndian> ElementSize;
};

template <bool IsBigEndian>
struct RankHeader {
    endian_specific_value<uint64_t, IsBigEndian> Coords[3];
    endian_specific_value<uint64_t, IsBigEndian> NElems;
    endian_specific_value<uint64_t, IsBigEndian> Start;
    endian_specific_value<uint64_t, IsBigEndian> GlobalRank;
};

static const size_t FilterNameSize = 8;
static const size_t MaxFilters = 4;
template <bool IsBigEndian>
struct BlockHeader {
    char Filters[MaxFilters][FilterNameSize];
    endian_specific_value<uint64_t, IsBigEndian> Start;
    endian_specific_value<uint64_t, IsBigEndian> Size;
};

template <bool IsBigEndian>
struct CompressHeader {
    endian_specific_value<uint64_t, IsBigEndian> OrigCRC;
};
const char *CompressName = "BLOSC";

#pragma pack()

unsigned GenericIO::DefaultFileIOType = FileIOPOSIX;
int GenericIO::DefaultPartition = 0;
bool GenericIO::DefaultShouldCompress = false;

  #ifndef GENERICIO_NO_MPI
    std::size_t GenericIO::CollectiveMPIIOThreshold = 0;
  #endif

static bool blosc_initialized = false;

#ifndef GENERICIO_NO_MPI
void GenericIO::write() {
    if (isBigEndian())
        write<true>();
    else
        write<false>();
}

// Note: writing errors are not currently recoverable (one rank may fail
// while the others don't).
template <bool IsBigEndian>
void GenericIO::write() {
    const char *Magic = IsBigEndian ? MagicBE : MagicLE;

    uint64_t FileSize = 0;

    int NRanks, Rank;
    MPI_Comm_rank(Comm, &Rank);
    MPI_Comm_size(Comm, &NRanks);

    #ifdef __bgq__
    MPI_Barrier(Comm);
    #endif
    MPI_Comm_split(Comm, Partition, Rank, &SplitComm);

    int SplitNRanks, SplitRank;
    MPI_Comm_rank(SplitComm, &SplitRank);
    MPI_Comm_size(SplitComm, &SplitNRanks);


    // Making a duplicate
    bool useDuplicateData = false;
    std::vector<GioData> _Vars;
    _Vars.resize(Vars.size());
    for (int i=0; i<Vars.size(); i++)
        _Vars[i].data = Vars[i].Data;


    string LocalFileName;
  if (SplitNRanks != NRanks) {
    if (Rank == 0) {
            // In split mode, the specified file becomes the rank map, and the real
            // data is partitioned.

            vector<int> MapRank, MapPartition;
            MapRank.resize(NRanks);
            for (int i = 0; i < NRanks; ++i) MapRank[i] = i;

            MapPartition.resize(NRanks);
            MPI_Gather(&Partition, 1, MPI_INT, &MapPartition[0], 1, MPI_INT, 0, Comm);

            GenericIO GIO(MPI_COMM_SELF, FileName, FileIOType);
            GIO.setNumElems(NRanks);
            GIO.addVariable("$rank", MapRank); /* this is for use by humans; the reading
                                            code assumes that the partitions are in
                                            rank order */
            GIO.addVariable("$partition", MapPartition);

            vector<int> CX, CY, CZ;
            int TopoStatus;
            MPI_Topo_test(Comm, &TopoStatus);
      if (TopoStatus == MPI_CART) {
                CX.resize(NRanks);
                CY.resize(NRanks);
                CZ.resize(NRanks);

        for (int i = 0; i < NRanks; ++i) {
                    int C[3];
                    MPI_Cart_coords(Comm, i, 3, C);

                    CX[i] = C[0];
                    CY[i] = C[1];
                    CZ[i] = C[2];
                }

                GIO.addVariable("$x", CX);
                GIO.addVariable("$y", CY);
                GIO.addVariable("$z", CZ);
            }

            GIO.write();
    } else {
            MPI_Gather(&Partition, 1, MPI_INT, 0, 0, MPI_INT, 0, Comm);
        }

        stringstream ss;
        ss << FileName << "#" << Partition;
        LocalFileName = ss.str();
  } else {
        LocalFileName = FileName;
    }

    RankHeader<IsBigEndian> RHLocal;
    int Dims[3], Periods[3], Coords[3];

    int TopoStatus;
    MPI_Topo_test(Comm, &TopoStatus);
  if (TopoStatus == MPI_CART) {
        MPI_Cart_get(Comm, 3, Dims, Periods, Coords);
  } else {
        Dims[0] = NRanks;
        std::fill(Dims + 1, Dims + 3, 1);
        std::fill(Periods, Periods + 3, 0);
        Coords[0] = Rank;
        std::fill(Coords + 1, Coords + 3, 0);
    }


    float myExtents[6];
    float physicalDims[3];
    for (int i=0; i<3; i++)
        physicalDims[i] = (PhysScale[i]-PhysOrigin[i])/Dims[i];

    for (int i=0; i<3; i++)
    {
        myExtents[i*2]     = Coords[i] * physicalDims[i];
        myExtents[i*2 + 1] = myExtents[i*2] + physicalDims[i];
    }

    std::copy(Coords, Coords + 3, RHLocal.Coords);
    RHLocal.NElems = NElems;
    RHLocal.Start = 0;
    RHLocal.GlobalRank = Rank;

    bool ShouldCompress = DefaultShouldCompress;
    const char *EnvStr = getenv("GENERICIO_COMPRESS");
  if (EnvStr) {
        int Mod = atoi(EnvStr);
        ShouldCompress = (Mod > 0);
    }

    bool NeedsBlockHeaders = ShouldCompress;
    EnvStr = getenv("GENERICIO_FORCE_BLOCKS");
  if (!NeedsBlockHeaders && EnvStr) {
        int Mod = atoi(EnvStr);
        NeedsBlockHeaders = (Mod > 0);
    }


    
    //Check if we can build an octree
    if (hasOctree)
    {
        bool foundx, foundy, foundz;
        foundx = foundy = foundz = false;
        
        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordX)
            {
                foundx = true;
                break;
            }


        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordY)
            {
                foundy = true;
                break;
            }


        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordZ)
            {
                foundz = true;
                break;
            }

        
        // If the data has no position data, we can't build an octree
        hasOctree = hasOctree && foundx;
        hasOctree = hasOctree && foundy;
        hasOctree = hasOctree && foundz;


        // Only makes sense to build octree if we are going to partition the ranks
        if (numOctreeLevels < 2)
            hasOctree = false;
    }

    //
    // Octree
    if (hasOctree)
    {
        Timer initOctreeClock, findLeafExtentClock, findPartitionClock, rearrageClock, gatherClock, createOctreeHeaderClock;

      Memory ongoingMem;          ongoingMem.start();
      Timer createOctreeClock;    createOctreeClock.start();
        

        #define DEBUG_ON 0
      #ifdef DEBUG_ON
        std::stringstream log;
      #endif

        size_t numParticles = NElems;   // num of particles for my rank

        //
        // MPI Rank 
        int myRank = Rank;
        int numRanks = NRanks;


        //
        // Simulation and rank extents
        float simExtents[6];
        simExtents[0] = PhysOrigin[0];  simExtents[1] = PhysScale[0];
        simExtents[2] = PhysOrigin[1];  simExtents[3] = PhysScale[1];
        simExtents[4] = PhysOrigin[2];  simExtents[5] = PhysScale[2];

        float physicalDims[3];
        for (int i=0; i<3; i++)
            physicalDims[i] = (PhysScale[i]-PhysOrigin[i])/Dims[i];

        float myRankExtents[6];
        for (int i=0; i<3; i++)
        {
            myRankExtents[i*2]     = Coords[i] * physicalDims[i];
            myRankExtents[i*2 + 1] = myRankExtents[i*2] + physicalDims[i];
        }

      
            
      initOctreeClock.start();

        //
        // Create octree structure
        Octree gioOctree(myRank, myRankExtents);
        gioOctree.init(numOctreeLevels, simExtents, Dims[0], Dims[1], Dims[2]); // num octree levels, sim extent, sim decompisition

      initOctreeClock.stop();
      findLeafExtentClock.start();

        //
        // Get the extents of each of my leaves
        std::vector<float> leavesExtentsVec = gioOctree.getMyLeavesExtent(myRankExtents, numOctreeLevels);
        int numleavesForMyRank = leavesExtentsVec.size()/6;
        float *leavesExtents = &leavesExtentsVec[0];

      findLeafExtentClock.stop();
      findPartitionClock.start();


        //
        // Find the partition for each particle
        // Use fisrt instance of position for octree
        float *_xx, *_yy, *_zz;
        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordX)
            {
                _xx = (float*)Vars[i].Data;
                break;
            }

        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordY)
            {
                _yy = (float*)Vars[i].Data;
                break;
            }

        for (size_t i = 0; i < Vars.size(); ++i)
            if (Vars[i].IsPhysCoordZ)
            {
                _zz = (float*)Vars[i].Data;
                break;
            }
    
        // Find the partition
        std::vector<int> leafPosition;                    // which leaf is the particle in
        std::vector<uint64_t> numParticlesForMyLeaf;      // #particles per leaf
        numParticlesForMyLeaf = gioOctree.findLeaf(_xx,_yy,_zz, numParticles, numleavesForMyRank, leavesExtents, leafPosition);


      findPartitionClock.stop();


      #ifdef DEBUG_ON
        log << "\nnumleavesForMyRank: " << numleavesForMyRank << std::endl;
        log << "numParticles: " << numParticles << "\n";
        log << "octreeLeafshuffle: " << octreeLeafshuffle << "\n";
        
        log << "\nOctree initialization took : " << initOctreeClock.getDuration() << " s\n";
        log << "Octree Find octree leaf extents took : " << findLeafExtentClock.getDuration() << " s\n";
        log << "Octree Find particle position took : " << findPartitionClock.getDuration() << " s\n";
        log << "\n|After findLeaf: " << ongoingMem.getMemoryInUseInMB() << " MB " << std::endl;
      #endif
      
      rearrageClock.start();


        //
        // Duplicate each of the variable
        useDuplicateData = true;
        for (size_t i = 0; i < Vars.size(); ++i)
        {
            _Vars[i].init(i, Vars[i].Name, static_cast<int>(Vars[i].Size), Vars[i].IsFloat, Vars[i].IsSigned, Vars[i].IsPhysCoordX, Vars[i].IsPhysCoordY, Vars[i].IsPhysCoordZ);
            _Vars[i].setNumElements(numParticles);
            _Vars[i].allocateMem(1);

            std::memcpy(_Vars[i].data, Vars[i].Data, numParticles*Vars[i].Size );

        }

        //
        // Rearrange the array based on leaves
        for (size_t i = 0; i < Vars.size(); ++i)
        {
            if (Vars[i].IsFloat)
            {
                float *_temp;
                _temp = (float*)_Vars[i].data;
                gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
            }
            else
                if (Vars[i].IsSigned)
                {
                    if (Vars[i].Size == 1)
                    {
                        int8_t *_temp;
                        _temp = (int8_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 2)
                    {
                        int16_t *_temp;
                        _temp = (int16_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 4)
                    {
                        int32_t *_temp;
                        _temp = (int32_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 8)
                    {
                        int64_t *_temp;
                        _temp = (int64_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                }
                else
                {
                    if (Vars[i].Size == 1)
                    {
                        uint8_t *_temp;
                        _temp = (uint8_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 2)
                    {
                        uint16_t *_temp;
                        _temp = (uint16_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 4)
                    {
                        uint32_t *_temp;
                        _temp = (uint32_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    else if (Vars[i].Size == 8)
                    {
                        uint64_t *_temp;
                        _temp = (uint64_t*)_Vars[i].data;
                        gioOctree.reorganizeArrayInPlace(numleavesForMyRank, numParticlesForMyLeaf, leafPosition, _temp, numParticles, octreeLeafshuffle);
                    }
                    
                }

          #ifdef DEBUG_ON
            log << gioOctree.getLog();
          #endif
        }

        leafPosition.clear();   leafPosition.shrink_to_fit();

      rearrageClock.stop(); 
      gatherClock.start();


        //
        // Gather num leaves each rank has
        int *numLeavesPerRank = new int[numRanks];
        MPI_Allgather( &numleavesForMyRank, 1, MPI_INT,  numLeavesPerRank, 1, MPI_INT,  MPI_COMM_WORLD);
    


        //
        // Gather num particles in each leaf for each rank
        int totalLeavesForSim = numLeavesPerRank[0];

        int *_offsets = new int[numRanks];
        _offsets[0] = 0;

        for (int i=1; i<numRanks; i++)
        {
            totalLeavesForSim += numLeavesPerRank[i];
            _offsets[i] = _offsets[i-1] + numLeavesPerRank[i-1];
        }

        uint64_t *numParticlesPerLeaf = new uint64_t[totalLeavesForSim];
        uint64_t *myLeavesCount = &numParticlesForMyLeaf[0];
        MPI_Allgatherv( myLeavesCount, numleavesForMyRank, MPI_UINT64_T,  numParticlesPerLeaf, numLeavesPerRank, _offsets, MPI_UINT64_T,  MPI_COMM_WORLD); 

        if (_offsets != NULL)
            delete []_offsets;
        _offsets = NULL;



        //
        // Gather extents for each rank        
        int *_extentsCountPerRank = new int[numRanks];
        int *_offsetsExtents = new int[numRanks];
        float *allOctreeLeavesExtents = new float[totalLeavesForSim*6];
        
        _offsetsExtents[0] = 0;
        _extentsCountPerRank[0] = numLeavesPerRank[0]*6;
        for (int r=1; r<numRanks; r++)
        {
            _extentsCountPerRank[r] = numLeavesPerRank[r]*6;
            _offsetsExtents[r] = _offsetsExtents[r-1] + numLeavesPerRank[r-1]*6;
        }
        
        MPI_Allgatherv(leavesExtents, numleavesForMyRank*6, MPI_FLOAT,  allOctreeLeavesExtents, _extentsCountPerRank, _offsetsExtents, MPI_FLOAT,  MPI_COMM_WORLD); 


        leavesExtentsVec.clear();   leavesExtentsVec.shrink_to_fit();
        leavesExtents = NULL;

        if (_offsetsExtents != NULL)
            delete []_offsetsExtents;
        _offsetsExtents = NULL;

        if (_extentsCountPerRank != NULL)
            delete []_extentsCountPerRank;
        _extentsCountPerRank = NULL;

      gatherClock.stop();
      createOctreeHeaderClock.start();
      

        if (SplitRank == 0)
        {
            //
            // Create Header info
            addOctreeHeader( (uint64_t)((int)octreeLeafshuffle), (uint64_t)numOctreeLevels, (uint64_t)totalLeavesForSim );

            //
            // Add Octree ranks
            int _leafCounter = 0;
            for (int r=0; r<numRanks; r++)
            {
                uint64_t offsetInRank = 0;
                for (int l=0; l<numLeavesPerRank[r]; l++)
                {
                    uint64_t _leafExtents[6];

                    for (int i=0; i<6; i++)
                        _leafExtents[i] = (uint64_t) round( allOctreeLeavesExtents[_leafCounter*6 + i] );
                                            
                    addOctreeRow(_leafCounter, _leafExtents, numParticlesPerLeaf[_leafCounter], offsetInRank, r);

                    offsetInRank += numParticlesPerLeaf[_leafCounter];
                    _leafCounter++;
                }
            }
        }

        

        if (numLeavesPerRank != NULL)
            delete []numLeavesPerRank;
        numLeavesPerRank = NULL;

        if (numParticlesPerLeaf != NULL)
            delete []numParticlesPerLeaf;
        numParticlesPerLeaf = NULL;

        if (allOctreeLeavesExtents != NULL)
            delete []allOctreeLeavesExtents;
        allOctreeLeavesExtents = NULL;


      createOctreeHeaderClock.stop();
      ongoingMem.stop();
      createOctreeClock.stop();


      #ifdef DEBUG_ON
        log << "\nOctree rearrage took : " << rearrageClock.getDuration() << " s\n";
        log << "Octree gathers took : " << gatherClock.getDuration() << " s\n";
        log << "Octree octree header creation took : " << createOctreeHeaderClock.getDuration() << " s\n";

        log << "\n|After, mem leaked: " << ongoingMem.getMemorySizeInMB() << " MB " << std::endl;
        log << "\n\nOctree processing took:: " << createOctreeClock.getDuration() << " s " << std::endl;
        writeLog("log_" + std::to_string(myRank) ,log.str());
      #endif
    }   // end octree
    


    vector<BlockHeader<IsBigEndian> > LocalBlockHeaders;
    vector<void *> LocalData;
    vector<bool> LocalHasExtraSpace;
    vector<vector<unsigned char> > LocalCData;
  if (NeedsBlockHeaders) {
        LocalBlockHeaders.resize(Vars.size());
        LocalData.resize(Vars.size());
        LocalHasExtraSpace.resize(Vars.size());
        if (ShouldCompress)
            LocalCData.resize(Vars.size());

    for (size_t i = 0; i < Vars.size(); ++i) {
            // Filters null by default, leave null starting address (needs to be
            // calculated by the header-writing rank).
            memset(&LocalBlockHeaders[i], 0, sizeof(BlockHeader<IsBigEndian>));
      if (ShouldCompress) {
                LocalCData[i].resize(sizeof(CompressHeader<IsBigEndian>));

                CompressHeader<IsBigEndian> *CH = (CompressHeader<IsBigEndian>*) &LocalCData[i][0];
                //CH->OrigCRC = crc64_omp(Vars[i].Data, Vars[i].Size * NElems);
                CH->OrigCRC = crc64_omp(_Vars[i].data, Vars[i].Size * NElems);

                #ifdef _OPENMP
                #pragma omp master
                {
                #endif

       if (!blosc_initialized) {
                        blosc_init();
                        blosc_initialized = true;
                    }

                    #ifdef _OPENMP
                    blosc_set_nthreads(omp_get_max_threads());
                }
                    #endif

                LocalCData[i].resize(LocalCData[i].size() + NElems * Vars[i].Size);
                // if (blosc_compress(9, 1, Vars[i].Size, NElems * Vars[i].Size, Vars[i].Data,
                //                    &LocalCData[i][0] + sizeof(CompressHeader<IsBigEndian>),
                //                    NElems * Vars[i].Size) <= 0)
                if (blosc_compress(9, 1, Vars[i].Size, NElems * Vars[i].Size, _Vars[i].data,
                                   &LocalCData[i][0] + sizeof(CompressHeader<IsBigEndian>),
                                   NElems * Vars[i].Size) <= 0)
                    goto nocomp;

                strncpy(LocalBlockHeaders[i].Filters[0], CompressName, FilterNameSize);
                size_t CNBytes, CCBytes, CBlockSize;
                blosc_cbuffer_sizes(&LocalCData[i][0] + sizeof(CompressHeader<IsBigEndian>),
                                    &CNBytes, &CCBytes, &CBlockSize);
                LocalCData[i].resize(CCBytes + sizeof(CompressHeader<IsBigEndian>));

                LocalBlockHeaders[i].Size = LocalCData[i].size();
                LocalCData[i].resize(LocalCData[i].size() + CRCSize);
                LocalData[i] = &LocalCData[i][0];
                LocalHasExtraSpace[i] = true;
      } else {
              nocomp:
                LocalBlockHeaders[i].Size = NElems * Vars[i].Size;
                //LocalData[i] = Vars[i].Data;
                LocalData[i] = _Vars[i].data;
                LocalHasExtraSpace[i] = Vars[i].HasExtraSpace;
            }
        }
    }

    double StartTime = MPI_Wtime();

    if (SplitRank == 0)
    {        
        std::string serializedOctree;
        uint64_t octreeSize = 0;
        uint64_t octreeStart = 0;
        if (hasOctree)
        {
            //std::cout << "Serializing octree" << std::endl;
            serializedOctree = octreeData.serialize(IsBigEndian);
            octreeSize  = serializedOctree.size();
        }

        uint64_t HeaderSize = sizeof(GlobalHeader<IsBigEndian>) + Vars.size() * sizeof(VariableHeader<IsBigEndian>) +
                              SplitNRanks * sizeof(RankHeader<IsBigEndian>) + CRCSize + octreeSize;

        if (NeedsBlockHeaders)
            HeaderSize += SplitNRanks * Vars.size() * sizeof(BlockHeader<IsBigEndian>) + octreeSize;

        vector<char> Header(HeaderSize, 0);
        GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &Header[0];
        std::copy(Magic, Magic + MagicSize, GH->Magic);
        GH->HeaderSize = HeaderSize - CRCSize;
        GH->NElems = NElems; // This will be updated later
        std::copy(Dims, Dims + 3, GH->Dims);
        GH->NVars = Vars.size();
        GH->VarsSize = sizeof(VariableHeader<IsBigEndian>);
        GH->VarsStart = sizeof(GlobalHeader<IsBigEndian>) + octreeSize;
        GH->NRanks = SplitNRanks;
        GH->RanksSize = sizeof(RankHeader<IsBigEndian>);
        GH->RanksStart = GH->VarsStart + Vars.size() * sizeof(VariableHeader<IsBigEndian>);
        GH->GlobalHeaderSize = sizeof(GlobalHeader<IsBigEndian>);
        std::copy(PhysOrigin, PhysOrigin + 3, GH->PhysOrigin);
        std::copy(PhysScale,  PhysScale  + 3, GH->PhysScale);


        if (hasOctree)
        {
            octreeStart = sizeof(GlobalHeader<IsBigEndian>);
            std::copy( serializedOctree.begin(), serializedOctree.end(), &Header[GH->GlobalHeaderSize] );
        }
        GH->OctreeSize = octreeSize;
        GH->OctreeStart = octreeStart;


        if (!NeedsBlockHeaders)
        {
            GH->BlocksSize = GH->BlocksStart = 0;
        }
        else
        {
            GH->BlocksSize = sizeof(BlockHeader<IsBigEndian>);
            GH->BlocksStart = GH->RanksStart + SplitNRanks * sizeof(RankHeader<IsBigEndian>);
        }

        uint64_t RecordSize = 0;
        VariableHeader<IsBigEndian> *VH = (VariableHeader<IsBigEndian> *) &Header[GH->VarsStart];
    for (size_t i = 0; i < Vars.size(); ++i, ++VH) {
            string VName(Vars[i].Name);
            VName.resize(NameSize);

            std::copy(VName.begin(), VName.end(), VH->Name);
            uint64_t VFlags = 0;
            if (Vars[i].IsFloat)  VFlags |= FloatValue;
            if (Vars[i].IsSigned) VFlags |= SignedValue;
            if (Vars[i].IsPhysCoordX) VFlags |= ValueIsPhysCoordX;
            if (Vars[i].IsPhysCoordY) VFlags |= ValueIsPhysCoordY;
            if (Vars[i].IsPhysCoordZ) VFlags |= ValueIsPhysCoordZ;
            if (Vars[i].MaybePhysGhost) VFlags |= ValueMaybePhysGhost;
            VH->Flags = VFlags;
            RecordSize += VH->Size = Vars[i].Size;
            VH->ElementSize = Vars[i].ElementSize;
        }

        MPI_Gather(&RHLocal, sizeof(RHLocal), MPI_BYTE,
                   &Header[GH->RanksStart], sizeof(RHLocal),
                   MPI_BYTE, 0, SplitComm);

    if (NeedsBlockHeaders) {
            MPI_Gather(&LocalBlockHeaders[0],
                       Vars.size()*sizeof(BlockHeader<IsBigEndian>), MPI_BYTE,
                       &Header[GH->BlocksStart],
                       Vars.size()*sizeof(BlockHeader<IsBigEndian>), MPI_BYTE,
                       0, SplitComm);

            BlockHeader<IsBigEndian> *BH = (BlockHeader<IsBigEndian> *) &Header[GH->BlocksStart];
            for (int i = 0; i < SplitNRanks; ++i)
      for (size_t j = 0; j < Vars.size(); ++j, ++BH) {
                    if (i == 0 && j == 0)
                        BH->Start = HeaderSize;
                    else
                        BH->Start = BH[-1].Start + BH[-1].Size + CRCSize;
                }

            RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &Header[GH->RanksStart];
            RH->Start = HeaderSize; ++RH;
      for (int i = 1; i < SplitNRanks; ++i, ++RH) {
                RH->Start =
                    ((BlockHeader<IsBigEndian> *) &Header[GH->BlocksStart])[i * Vars.size()].Start;
                GH->NElems += RH->NElems;
            }

            // Compute the total file size.
            uint64_t LastData = BH[-1].Size + CRCSize;
            FileSize = BH[-1].Start + LastData;
    } else {
            RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &Header[GH->RanksStart];
            RH->Start = HeaderSize; ++RH;
      for (int i = 1; i < SplitNRanks; ++i, ++RH) {
                uint64_t PrevNElems = RH[-1].NElems;
                uint64_t PrevData = PrevNElems * RecordSize + CRCSize * Vars.size();
                RH->Start = RH[-1].Start + PrevData;
                GH->NElems += RH->NElems;
            }

            // Compute the total file size.
            uint64_t LastNElems = RH[-1].NElems;
            uint64_t LastData = LastNElems * RecordSize + CRCSize * Vars.size();
            FileSize = RH[-1].Start + LastData;
        }

        // Now that the starting offset has been computed, send it back to each rank.
        MPI_Scatter(&Header[GH->RanksStart], sizeof(RHLocal),
                    MPI_BYTE, &RHLocal, sizeof(RHLocal),
                    MPI_BYTE, 0, SplitComm);

        if (NeedsBlockHeaders)
            MPI_Scatter(&Header[GH->BlocksStart],
                        sizeof(BlockHeader<IsBigEndian>)*Vars.size(), MPI_BYTE,
                        &LocalBlockHeaders[0],
                        sizeof(BlockHeader<IsBigEndian>)*Vars.size(), MPI_BYTE,
                        0, SplitComm);

        uint64_t HeaderCRC = crc64_omp(&Header[0], HeaderSize - CRCSize);
        crc64_invert(HeaderCRC, &Header[HeaderSize - CRCSize]);

        if (FileIOType == FileIOMPI)
            FH.get() = new GenericFileIO_MPI(MPI_COMM_SELF);
        else if (FileIOType == FileIOMPICollective)
            FH.get() = new GenericFileIO_MPICollective(MPI_COMM_SELF);
        else
            FH.get() = new GenericFileIO_POSIX();

        FH.get()->open(LocalFileName);
        FH.get()->setSize(FileSize);
        FH.get()->write(&Header[0], HeaderSize, 0, "header");

        close();
  } else {
        MPI_Gather(&RHLocal, sizeof(RHLocal), MPI_BYTE, 0, 0, MPI_BYTE, 0, SplitComm);
        if (NeedsBlockHeaders)
            MPI_Gather(&LocalBlockHeaders[0], Vars.size()*sizeof(BlockHeader<IsBigEndian>),
                       MPI_BYTE, 0, 0, MPI_BYTE, 0, SplitComm);
        MPI_Scatter(0, 0, MPI_BYTE, &RHLocal, sizeof(RHLocal), MPI_BYTE, 0, SplitComm);
        if (NeedsBlockHeaders)
            MPI_Scatter(0, 0, MPI_BYTE, &LocalBlockHeaders[0], sizeof(BlockHeader<IsBigEndian>)*Vars.size(),
                        MPI_BYTE, 0, SplitComm);
    }

    MPI_Barrier(SplitComm);

    if (FileIOType == FileIOMPI)
        FH.get() = new GenericFileIO_MPI(SplitComm);
    else if (FileIOType == FileIOMPICollective)
        FH.get() = new GenericFileIO_MPICollective(SplitComm);
    else
        FH.get() = new GenericFileIO_POSIX();

    FH.get()->open(LocalFileName);

    uint64_t Offset = RHLocal.Start;
  for (size_t i = 0; i < Vars.size(); ++i) {
        uint64_t WriteSize = NeedsBlockHeaders ?
                             LocalBlockHeaders[i].Size : NElems * Vars[i].Size;
        //void *Data = NeedsBlockHeaders ? LocalData[i] : Vars[i].Data;
        void *Data = NeedsBlockHeaders ? LocalData[i] : _Vars[i].data;
        uint64_t CRC = crc64_omp(Data, WriteSize);
        bool HasExtraSpace = NeedsBlockHeaders ?
                             LocalHasExtraSpace[i] : Vars[i].HasExtraSpace;
        char *CRCLoc = HasExtraSpace ?  ((char *) Data) + WriteSize : (char *) &CRC;

        if (NeedsBlockHeaders)
            Offset = LocalBlockHeaders[i].Start;

        // When using extra space for the CRC write, preserve the original contents.
        char CRCSave[CRCSize];
        if (HasExtraSpace)
            std::copy(CRCLoc, CRCLoc + CRCSize, CRCSave);

        crc64_invert(CRC, CRCLoc);

    if (HasExtraSpace) {
            FH.get()->write(Data, WriteSize + CRCSize, Offset, Vars[i].Name + " with CRC");
    } else {
            FH.get()->write(Data, WriteSize, Offset, Vars[i].Name);
            FH.get()->write(CRCLoc, CRCSize, Offset + WriteSize, Vars[i].Name + " CRC");
        }

        if (HasExtraSpace)
            std::copy(CRCSave, CRCSave + CRCSize, CRCLoc);

        Offset += WriteSize + CRCSize;
    }

    close();
    MPI_Barrier(Comm);

    double EndTime = MPI_Wtime();
    double TotalTime = EndTime - StartTime;
    double MaxTotalTime;
    MPI_Reduce(&TotalTime, &MaxTotalTime, 1, MPI_DOUBLE, MPI_MAX, 0, Comm);

  if (SplitNRanks != NRanks) {
        uint64_t ContribFileSize = (SplitRank == 0) ? FileSize : 0;
        MPI_Reduce(&ContribFileSize, &FileSize, 1, MPI_UINT64_T, MPI_SUM, 0, Comm);
    }

  if (Rank == 0) {
        double Rate = ((double) FileSize) / MaxTotalTime / (1024.*1024.);
        cout << "Wrote " << Vars.size() << " variables to " << FileName <<
             " (" << FileSize << " bytes) in " << MaxTotalTime << "s: " <<
             Rate << " MB/s" << endl;
    }


    if (useDuplicateData)
    {
        for (int i=0; i<Vars.size(); i++)
            _Vars[i].deAllocateMem();
    }

    MPI_Comm_free(&SplitComm);
    SplitComm = MPI_COMM_NULL;
}
#endif // GENERICIO_NO_MPI

template <bool IsBigEndian>
void GenericIO::readHeaderLeader(void *GHPtr, MismatchBehavior MB, int NRanks,
                                 int Rank, int SplitNRanks,
                                 string &LocalFileName, uint64_t &HeaderSize,
                                 vector<char> &Header) {
    GlobalHeader<IsBigEndian> &GH = *(GlobalHeader<IsBigEndian> *) GHPtr;

  if (MB == MismatchDisallowed) {
    if (SplitNRanks != (int) GH.NRanks) {
            stringstream ss;
            ss << "Won't read " << LocalFileName << ": communicator-size mismatch: " <<
               "current: " << SplitNRanks << ", file: " << GH.NRanks;
            throw runtime_error(ss.str());
        }

        #ifndef GENERICIO_NO_MPI
        int TopoStatus;
        MPI_Topo_test(Comm, &TopoStatus);
    if (TopoStatus == MPI_CART) {
            int Dims[3], Periods[3], Coords[3];
            MPI_Cart_get(Comm, 3, Dims, Periods, Coords);

            bool DimsMatch = true;
      for (int i = 0; i < 3; ++i) {
        if ((uint64_t) Dims[i] != GH.Dims[i]) {
                    DimsMatch = false;
                    break;
                }
            }

      if (!DimsMatch) {
                stringstream ss;
                ss << "Won't read " << LocalFileName <<
                   ": communicator-decomposition mismatch: " <<
                   "current: " << Dims[0] << "x" << Dims[1] << "x" << Dims[2] <<
                   ", file: " << GH.Dims[0] << "x" << GH.Dims[1] << "x" <<
                   GH.Dims[2];
                throw runtime_error(ss.str());
            }
        }
        #endif
  } else if (MB == MismatchRedistribute && !Redistributing) {
        Redistributing = true;

        int NFileRanks = RankMap.empty() ? (int) GH.NRanks : (int) RankMap.size();
        int NFileRanksPerRank = NFileRanks / NRanks;
        int NRemFileRank = NFileRanks % NRanks;

    if (!NFileRanksPerRank) {
            // We have only the remainder, so the last NRemFileRank ranks get one
            // file rank, and the others don't.
            if (NRemFileRank && NRanks - Rank <= NRemFileRank)
                SourceRanks.push_back(NRanks - (Rank + 1));
    } else {
            // Since NRemFileRank < NRanks, and we don't want to put any extra memory
            // load on rank 0 (because rank 0's memory load is normally higher than
            // the other ranks anyway), the last NRemFileRank will each take
            // (NFileRanksPerRank+1) file ranks.

            int FirstFileRank = 0, LastFileRank = NFileRanksPerRank - 1;
      for (int i = 1; i <= Rank; ++i) {
                FirstFileRank = LastFileRank + 1;
                LastFileRank  = FirstFileRank + NFileRanksPerRank - 1;

                if (NRemFileRank && NRanks - i <= NRemFileRank)
                    ++LastFileRank;
            }

            for (int i = FirstFileRank; i <= LastFileRank; ++i)
                SourceRanks.push_back(i);
        }
    }
    
    HeaderSize = GH.HeaderSize;
    Header.resize(HeaderSize + CRCSize, 0xFE /* poison */);
    FH.get()->read(&Header[0], HeaderSize + CRCSize, 0, "header");

    uint64_t CRC = crc64_omp(&Header[0], HeaderSize + CRCSize);
  if (CRC != (uint64_t) -1) {
        throw runtime_error("Header CRC check failed: " + LocalFileName);
    }
}

void GenericIO::readOctreeHeader(int octreeOffset, int octreeStringSize, bool bigEndian)
{
    std::vector<char> octreeHeader;
    octreeHeader.resize(octreeStringSize);
    FH.get()->read(&octreeHeader[0], octreeStringSize, octreeOffset, "Octree Header");

    octreeData.deserialize(&octreeHeader[0], bigEndian);
}

// Note: Errors from this function should be recoverable. This means that if
// one rank throws an exception, then all ranks should.
void GenericIO::openAndReadHeader(MismatchBehavior MB, int EffRank, bool CheckPartMap){
    int NRanks, Rank;
    #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(Comm, &Rank);
    MPI_Comm_size(Comm, &NRanks);
    #else
    Rank = 0;
    NRanks = 1;
    #endif

    if (EffRank == -1)
        EffRank = MB == MismatchRedistribute ? 0 : Rank;

  if (RankMap.empty() && CheckPartMap) {
        // First, check to see if the file is a rank map.
        unsigned long RanksInMap = 0;
    if (Rank == 0) {
      try {
                #ifndef GENERICIO_NO_MPI
                GenericIO GIO(MPI_COMM_SELF, FileName, FileIOType);
                #else
                GenericIO GIO(FileName, FileIOType);
                #endif
                GIO.openAndReadHeader(MismatchDisallowed, 0, false);
                RanksInMap = GIO.readNumElems();

                RankMap.resize(RanksInMap + GIO.requestedExtraSpace() / sizeof(int));
                GIO.addVariable("$partition", RankMap, true);

                GIO.readData(0, false);
                RankMap.resize(RanksInMap);
      } catch (...) {
                RankMap.clear();
                RanksInMap = 0;
            }
        }

        #ifndef GENERICIO_NO_MPI
        MPI_Bcast(&RanksInMap, 1, MPI_UNSIGNED_LONG, 0, Comm);
    if (RanksInMap > 0) {
            RankMap.resize(RanksInMap);
            MPI_Bcast(&RankMap[0], RanksInMap, MPI_INT, 0, Comm);
        }
        #endif
    }

    #ifndef GENERICIO_NO_MPI
    if (SplitComm != MPI_COMM_NULL)
        MPI_Comm_free(&SplitComm);
    #endif

    string LocalFileName;
  if (RankMap.empty()) {
        LocalFileName = FileName;
        #ifndef GENERICIO_NO_MPI
        MPI_Comm_dup(MB == MismatchRedistribute ? MPI_COMM_SELF : Comm, &SplitComm);
        #endif
  } else {
        stringstream ss;
        ss << FileName << "#" << RankMap[EffRank];
        LocalFileName = ss.str();
        #ifndef GENERICIO_NO_MPI
    if (MB == MismatchRedistribute) {
            MPI_Comm_dup(MPI_COMM_SELF, &SplitComm);
    } else {
            #ifdef __bgq__
            MPI_Barrier(Comm);
            #endif
            MPI_Comm_split(Comm, RankMap[EffRank], Rank, &SplitComm);
        }
        #endif
    }

    if (LocalFileName == OpenFileName)
        return;
    FH.close();

    int SplitNRanks, SplitRank;
    #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(SplitComm, &SplitRank);
    MPI_Comm_size(SplitComm, &SplitNRanks);
    #else
    SplitRank = 0;
    SplitNRanks = 1;
    #endif

    uint64_t HeaderSize;
    vector<char> Header;

  if (SplitRank == 0) {
      #ifndef GENERICIO_NO_MPI
        if (FileIOType == FileIOMPI)
            FH.get() = new GenericFileIO_MPI(MPI_COMM_SELF);
        else if (FileIOType == FileIOMPICollective)
            FH.get() = new GenericFileIO_MPICollective(MPI_COMM_SELF);
        else
      #endif
            FH.get() = new GenericFileIO_POSIX();

      #ifndef GENERICIO_NO_MPI
        char True = 1, False = 0;
      #endif

    try {
            FH.get()->open(LocalFileName, true);

            GlobalHeader<false> GH; // endianness does not matter yet...
            FH.get()->read(&GH, sizeof(GlobalHeader<false>), 0, "global header");

      if (string(GH.Magic, GH.Magic + MagicSize - 1) == MagicLE) {
                readHeaderLeader<false>(&GH, MB, NRanks, Rank, SplitNRanks, LocalFileName,
                                        HeaderSize, Header);
      } else if (string(GH.Magic, GH.Magic + MagicSize - 1) == MagicBE) {
                readHeaderLeader<true>(&GH, MB, NRanks, Rank, SplitNRanks, LocalFileName,
                                       HeaderSize, Header);
      } else {
                string Error = "invalid file-type identifier";
                throw runtime_error("Won't read " + LocalFileName + ": " + Error);
            }

          #ifndef GENERICIO_NO_MPI
            close();
            MPI_Bcast(&True, 1, MPI_BYTE, 0, SplitComm);
          #endif
    } catch (...) {
          #ifndef GENERICIO_NO_MPI
            MPI_Bcast(&False, 1, MPI_BYTE, 0, SplitComm);
          #endif
            close();
            throw;
        }
  } else {
      #ifndef GENERICIO_NO_MPI
        char Okay;
        MPI_Bcast(&Okay, 1, MPI_BYTE, 0, SplitComm);
        if (!Okay)
            throw runtime_error("Failure broadcast from rank 0");
      #endif
    }

  #ifndef GENERICIO_NO_MPI
    MPI_Bcast(&HeaderSize, 1, MPI_UINT64_T, 0, SplitComm);
  #endif

    Header.resize(HeaderSize, 0xFD /* poison */);
  #ifndef GENERICIO_NO_MPI
    MPI_Bcast(&Header[0], HeaderSize, MPI_BYTE, 0, SplitComm);
  #endif

  FH.getHeaderCache().clear();

    GlobalHeader<false> *GH = (GlobalHeader<false> *) &Header[0];



    // Read Octree info if one is present
    Timer readOctreeClock;
    readOctreeClock.start();

    int octreeStart = 0;
    int octreeSize = 0;
    if (GH->VarsStart != 168)      // for files that do not have octrees
        if (GH->OctreeSize != 0)   // for files with octree support but have no octree in place
        {
            bool isBigEndian = string(GH->Magic, GH->Magic + MagicSize - 1) == MagicBE;
            hasOctree = true;
            octreeSize = GH->OctreeSize;
            octreeStart = GH->OctreeStart;
            
            readOctreeHeader(octreeStart, octreeSize, isBigEndian);
        }
    readOctreeClock.stop();
    

    FH.setIsBigEndian(string(GH->Magic, GH->Magic + MagicSize - 1) == MagicBE);

    FH.getHeaderCache().swap(Header);
    OpenFileName = LocalFileName;

    #ifndef GENERICIO_NO_MPI
    if (!DisableCollErrChecking)
        MPI_Barrier(Comm);

  if (FileIOType == FileIOMPI)
    FH.get() = new GenericFileIO_MPI(SplitComm);
  else if (FileIOType == FileIOMPICollective)
    FH.get() = new GenericFileIO_MPICollective(SplitComm);
  else
    FH.get() = new GenericFileIO_POSIX();

    int OpenErr = 0, TotOpenErr;
  try {
        FH.get()->open(LocalFileName, true);
        MPI_Allreduce(&OpenErr, &TotOpenErr, 1, MPI_INT, MPI_SUM,
                      DisableCollErrChecking ? MPI_COMM_SELF : Comm);
  } catch (...) {
        OpenErr = 1;
        MPI_Allreduce(&OpenErr, &TotOpenErr, 1, MPI_INT, MPI_SUM,
                      DisableCollErrChecking ? MPI_COMM_SELF : Comm);
        throw;
    }

  if (TotOpenErr > 0) {
        stringstream ss;
        ss << TotOpenErr << " ranks failed to open file: " << LocalFileName;
        throw runtime_error(ss.str());
    }
    #endif
}

int GenericIO::readNRanks() {
    if (FH.isBigEndian())
        return readNRanks<true>();
    return readNRanks<false>();
}

template <bool IsBigEndian>
int GenericIO::readNRanks() {
    if (RankMap.size())
        return RankMap.size();

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");
    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    return (int) GH->NRanks;
}

void GenericIO::readDims(int Dims[3]) {
    if (FH.isBigEndian())
        readDims<true>(Dims);
    else
        readDims<false>(Dims);
}

template <bool IsBigEndian>
void GenericIO::readDims(int Dims[3]) {
    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");
    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    std::copy(GH->Dims, GH->Dims + 3, Dims);
}

uint64_t GenericIO::readTotalNumElems() {
    if (FH.isBigEndian())
        return readTotalNumElems<true>();
    return readTotalNumElems<false>();
}

template <bool IsBigEndian>
uint64_t GenericIO::readTotalNumElems() {
    if (RankMap.size())
        return (uint64_t) - 1;

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");
    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    return GH->NElems;
}

void GenericIO::readPhysOrigin(double Origin[3]) {
    if (FH.isBigEndian())
        readPhysOrigin<true>(Origin);
    else
        readPhysOrigin<false>(Origin);
}

// Define a "safe" version of offsetof (offsetof itself might not work for
// non-POD types, and at least xlC v12.1 will complain about this if you try).
#define offsetof_safe(S, F) (size_t(&(S)->F) - size_t(S))

template <bool IsBigEndian>
void GenericIO::readPhysOrigin(double Origin[3]) {
    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");
    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
  if (offsetof_safe(GH, PhysOrigin) >= GH->GlobalHeaderSize) {
        std::fill(Origin, Origin + 3, 0.0);
        return;
    }

    std::copy(GH->PhysOrigin, GH->PhysOrigin + 3, Origin);
}

void GenericIO::readPhysScale(double Scale[3]) {
    if (FH.isBigEndian())
        readPhysScale<true>(Scale);
    else
        readPhysScale<false>(Scale);
}

template <bool IsBigEndian>
void GenericIO::readPhysScale(double Scale[3]) {
    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");
    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
  if (offsetof_safe(GH, PhysScale) >= GH->GlobalHeaderSize) {
        std::fill(Scale, Scale + 3, 0.0);
        return;
    }

    std::copy(GH->PhysScale, GH->PhysScale + 3, Scale);
}

template <bool IsBigEndian>
static size_t getRankIndex(int EffRank, GlobalHeader<IsBigEndian> *GH,
                           vector<int> &RankMap, vector<char> &HeaderCache) {
    if (RankMap.empty())
        return EffRank;

  for (size_t i = 0; i < GH->NRanks; ++i) {
        RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &HeaderCache[GH->RanksStart +
                                      i * GH->RanksSize];
        if (offsetof_safe(RH, GlobalRank) >= GH->RanksSize)
            return EffRank;

        if ((int) RH->GlobalRank == EffRank)
            return i;
    }

    assert(false && "Index requested of an invalid rank");
    return (size_t) - 1;
}

int GenericIO::readGlobalRankNumber(int EffRank) {
    if (FH.isBigEndian())
        return readGlobalRankNumber<true>(EffRank);
    return readGlobalRankNumber<false>(EffRank);
}

template <bool IsBigEndian>
int GenericIO::readGlobalRankNumber(int EffRank) {
  if (EffRank == -1) {
        #ifndef GENERICIO_NO_MPI
        MPI_Comm_rank(Comm, &EffRank);
        #else
        EffRank = 0;
        #endif
    }

    openAndReadHeader(MismatchAllowed, EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];

    if (offsetof_safe(RH, GlobalRank) >= GH->RanksSize)
        return EffRank;

    return (int) RH->GlobalRank;
}

void GenericIO::getSourceRanks(vector<int> &SR) {
    SR.clear();

  if (Redistributing) {
        std::copy(SourceRanks.begin(), SourceRanks.end(), std::back_inserter(SR));
        return;
    }

    int Rank;
    #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(Comm, &Rank);
    #else
    Rank = 0;
    #endif

    SR.push_back(Rank);
}

size_t GenericIO::readNumElems(int EffRank) {
  if (EffRank == -1 && Redistributing) {
        DisableCollErrChecking = true;

        size_t TotalSize = 0;
        for (int i = 0, ie = SourceRanks.size(); i != ie; ++i)
            TotalSize += readNumElems(SourceRanks[i]);

        DisableCollErrChecking = false;
        return TotalSize;
    }

    if (FH.isBigEndian())
        return readNumElems<true>(EffRank);
    return readNumElems<false>(EffRank);
}

template <bool IsBigEndian>
size_t GenericIO::readNumElems(int EffRank) {
  if (EffRank == -1) {
        #ifndef GENERICIO_NO_MPI
        MPI_Comm_rank(Comm, &EffRank);
        #else
        EffRank = 0;
        #endif
    }

    openAndReadHeader(Redistributing ? MismatchRedistribute : MismatchAllowed,
                      EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];
    return (size_t) RH->NElems;
}

void GenericIO::readCoords(int Coords[3], int EffRank) {
  if (EffRank == -1 && Redistributing) {
        std::fill(Coords, Coords + 3, 0);
        return;
    }

    if (FH.isBigEndian())
        readCoords<true>(Coords, EffRank);
    else
        readCoords<false>(Coords, EffRank);
}

template <bool IsBigEndian>
void GenericIO::readCoords(int Coords[3], int EffRank) {
  if (EffRank == -1) {
        #ifndef GENERICIO_NO_MPI
        MPI_Comm_rank(Comm, &EffRank);
        #else
        EffRank = 0;
        #endif
    }

    openAndReadHeader(MismatchAllowed, EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];

    std::copy(RH->Coords, RH->Coords + 3, Coords);
}




void GenericIO::getVariableInfo(vector<VariableInfo> &VI)
{
    if (FH.isBigEndian())
        getVariableInfo<true>(VI);
    else
        getVariableInfo<false>(VI);
}

template <bool IsBigEndian>
void GenericIO::getVariableInfo(vector<VariableInfo> &VI)
{
    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    for (uint64_t j = 0; j < GH->NVars; ++j)
    {
        VariableHeader<IsBigEndian> *VH = (VariableHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->VarsStart +
                                          j * GH->VarsSize];

        string VName(VH->Name, VH->Name + NameSize);
        size_t VNameNull = VName.find('\0');
        if (VNameNull < NameSize)
            VName.resize(VNameNull);

        size_t ElementSize = VH->Size;
        if (offsetof_safe(VH, ElementSize) < GH->VarsSize)
            ElementSize = VH->ElementSize;

        bool IsFloat = (bool) (VH->Flags & FloatValue),
             IsSigned = (bool) (VH->Flags & SignedValue),
             IsPhysCoordX = (bool) (VH->Flags & ValueIsPhysCoordX),
             IsPhysCoordY = (bool) (VH->Flags & ValueIsPhysCoordY),
             IsPhysCoordZ = (bool) (VH->Flags & ValueIsPhysCoordZ),
             MaybePhysGhost = (bool) (VH->Flags & ValueMaybePhysGhost);
        VI.push_back(VariableInfo(VName, (size_t) VH->Size, IsFloat, IsSigned,
                                  IsPhysCoordX, IsPhysCoordY, IsPhysCoordZ,
                                  MaybePhysGhost, ElementSize));
    }
}

void GenericIO::setNaturalDefaultPartition()
{
    #ifdef __bgq__
    DefaultPartition = MPIX_IO_link_id();
    #else
    #ifndef GENERICIO_NO_MPI
    bool UseName = true;
    const char *EnvStr = getenv("GENERICIO_PARTITIONS_USE_NAME");
    if (EnvStr)
    {
        int Mod = atoi(EnvStr);
        UseName = (Mod != 0);
    }

    if (UseName)
    {
        // This is a heuristic to generate ~256 partitions based on the
        // names of the nodes.
        char Name[MPI_MAX_PROCESSOR_NAME];
        int Len = 0;

        MPI_Get_processor_name(Name, &Len);
        unsigned char color = 0;
        for (int i = 0; i < Len; ++i)
            color += (unsigned char) Name[i];

        DefaultPartition = color;
    }

    // This is for debugging.
    EnvStr = getenv("GENERICIO_RANK_PARTITIONS");
    if (EnvStr)
    {
        int Mod = atoi(EnvStr);
        if (Mod > 0)
        {
            int Rank;
            MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
            DefaultPartition += Rank % Mod;
        }
    }
    #endif
    #endif
}



void GenericIO::readData(int EffRank, bool PrintStats, bool CollStats){
    int Rank;
  #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(Comm, &Rank);
  #else
    Rank = 0;
  #endif

    uint64_t TotalReadSize = 0;
  #ifndef GENERICIO_NO_MPI
    double StartTime = MPI_Wtime();
  #else
    double StartTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    int NErrs[3] = { 0, 0, 0 };

  if (EffRank == -1 && Redistributing) {
        DisableCollErrChecking = true;

        size_t RowOffset = 0;
    for (int i = 0, ie = SourceRanks.size(); i != ie; ++i) {
            readData(SourceRanks[i], RowOffset, Rank, TotalReadSize, NErrs);
            RowOffset += readNumElems(SourceRanks[i]);
        }

        DisableCollErrChecking = false;
  } else {
        readData(EffRank, 0, Rank, TotalReadSize, NErrs);
    }

    int AllNErrs[3];
  #ifndef GENERICIO_NO_MPI
    MPI_Allreduce(NErrs, AllNErrs, 3, MPI_INT, MPI_SUM, Comm);
  #else
    AllNErrs[0] = NErrs[0]; AllNErrs[1] = NErrs[1]; AllNErrs[2] = NErrs[2];
  #endif

  if (AllNErrs[0] > 0 || AllNErrs[1] > 0 || AllNErrs[2] > 0) {
        stringstream ss;
        ss << "Experienced " << AllNErrs[0] << " I/O error(s), " <<
           AllNErrs[1] << " CRC error(s) and " << AllNErrs[2] <<
           " decompression CRC error(s) reading: " << OpenFileName;
        throw runtime_error(ss.str());
    }

  #ifndef GENERICIO_NO_MPI
    MPI_Barrier(Comm);
  #endif

  #ifndef GENERICIO_NO_MPI
    double EndTime = MPI_Wtime();
  #else
    double EndTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    double TotalTime = EndTime - StartTime;
    double MaxTotalTime;
  #ifndef GENERICIO_NO_MPI
    if (CollStats)
        MPI_Reduce(&TotalTime, &MaxTotalTime, 1, MPI_DOUBLE, MPI_MAX, 0, Comm);
    else
  #endif
        MaxTotalTime = TotalTime;

    uint64_t AllTotalReadSize;
  #ifndef GENERICIO_NO_MPI
    if (CollStats)
        MPI_Reduce(&TotalReadSize, &AllTotalReadSize, 1, MPI_UINT64_T, MPI_SUM, 0, Comm);
    else
  #endif
        AllTotalReadSize = TotalReadSize;

  if (Rank == 0 && PrintStats) {
        double Rate = ((double) AllTotalReadSize) / MaxTotalTime / (1024.*1024.);
        cout << "Read " << Vars.size() << " variables from " << FileName <<
             " (" << AllTotalReadSize << " bytes) in " << MaxTotalTime << "s: " <<
             Rate << " MB/s [excluding header read]" << endl;
    }
}

void GenericIO::readData(int EffRank, size_t RowOffset, int Rank,
                         uint64_t &TotalReadSize, int NErrs[3]) {
    if (FH.isBigEndian())
        readData<true>(EffRank, RowOffset, Rank, TotalReadSize, NErrs);
    else
        readData<false>(EffRank, RowOffset, Rank, TotalReadSize, NErrs);
}

// Note: Errors from this function should be recoverable. This means that if
// one rank throws an exception, then all ranks should.
template <bool IsBigEndian>
void GenericIO::readData(int EffRank, size_t RowOffset, int Rank,
                         uint64_t &TotalReadSize, int NErrs[3]) {
    openAndReadHeader(Redistributing ? MismatchRedistribute : MismatchAllowed,
                      EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    if (EffRank == -1)
        EffRank = Rank;

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];

  for (size_t i = 0; i < Vars.size(); ++i) {
        uint64_t Offset = RH->Start;
        bool VarFound = false;
    for (uint64_t j = 0; j < GH->NVars; ++j) {
            VariableHeader<IsBigEndian> *VH = (VariableHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->VarsStart +
                                              j * GH->VarsSize];

            string VName(VH->Name, VH->Name + NameSize);
            size_t VNameNull = VName.find('\0');
            if (VNameNull < NameSize)
                VName.resize(VNameNull);

            uint64_t ReadSize = RH->NElems * VH->Size + CRCSize;
      if (VName != Vars[i].Name) {
                Offset += ReadSize;
                continue;
            }

            size_t ElementSize = VH->Size;
            if (offsetof_safe(VH, ElementSize) < GH->VarsSize)
                ElementSize = VH->ElementSize;

            VarFound = true;
            bool IsFloat = (bool) (VH->Flags & FloatValue),
                 IsSigned = (bool) (VH->Flags & SignedValue);
      if (VH->Size != Vars[i].Size) {
                stringstream ss;
                ss << "Size mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " << Vars[i].Size <<
                   ", file: " << VH->Size;
                throw runtime_error(ss.str());
      } else if (ElementSize != Vars[i].ElementSize) {
                stringstream ss;
                ss << "Element size mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " << Vars[i].ElementSize <<
                   ", file: " << ElementSize;
                throw runtime_error(ss.str());
      } else if (IsFloat != Vars[i].IsFloat) {
                string Float("float"), Int("integer");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsFloat ? Float : Int) <<
                   ", file: " << (IsFloat ? Float : Int);
                throw runtime_error(ss.str());
      } else if (IsSigned != Vars[i].IsSigned) {
                string Signed("signed"), Uns("unsigned");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsSigned ? Signed : Uns) <<
                   ", file: " << (IsSigned ? Signed : Uns);
                throw runtime_error(ss.str());
            }

            size_t VarOffset = RowOffset * Vars[i].Size;
            void *VarData = ((char *) Vars[i].Data) + VarOffset;

            vector<unsigned char> LData;
            void *Data = VarData;
            bool HasExtraSpace = Vars[i].HasExtraSpace;
            if (offsetof_safe(GH, BlocksStart) < GH->GlobalHeaderSize &&
          GH->BlocksSize > 0) {
                BlockHeader<IsBigEndian> *BH = (BlockHeader<IsBigEndian> *)
                                               &FH.getHeaderCache()[GH->BlocksStart +
                                                       (RankIndex * GH->NVars + j) * GH->BlocksSize];
                ReadSize = BH->Size + CRCSize;
                Offset = BH->Start;

        if (strncmp(BH->Filters[0], CompressName, FilterNameSize) == 0) {
                    LData.resize(ReadSize);
                    Data = &LData[0];
                    HasExtraSpace = true;
        } else if (BH->Filters[0][0] != '\0') {
                    stringstream ss;
                    ss << "Unknown filter \"" << BH->Filters[0] << "\" on variable " << Vars[i].Name;
                    throw runtime_error(ss.str());
                }
            }

            assert(HasExtraSpace && "Extra space required for reading");

            char CRCSave[CRCSize];
            char *CRCLoc = ((char *) Data) + ReadSize - CRCSize;
            if (HasExtraSpace)
                std::copy(CRCLoc, CRCLoc + CRCSize, CRCSave);

            int Retry = 0;
            {
                int RetryCount = 300;
                const char *EnvStr = getenv("GENERICIO_RETRY_COUNT");
                if (EnvStr)
                    RetryCount = atoi(EnvStr);

                int RetrySleep = 100; // ms
                EnvStr = getenv("GENERICIO_RETRY_SLEEP");
                if (EnvStr)
                    RetrySleep = atoi(EnvStr);

        for (; Retry < RetryCount; ++Retry) {
          try {
                        FH.get()->read(Data, ReadSize, Offset, Vars[i].Name);
                        break;
          } catch (...) { }

                    usleep(1000 * RetrySleep);
                }

        if (Retry == RetryCount) {
                    ++NErrs[0];
                    break;
        } else if (Retry > 0) {
                    EnvStr = getenv("GENERICIO_VERBOSE");
          if (EnvStr) {
                        int Mod = atoi(EnvStr);
            if (Mod > 0) {
                            int Rank;
                            #ifndef GENERICIO_NO_MPI
                            MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
                            #else
                            Rank = 0;
                            #endif

                            std::cerr << "Rank " << Rank << ": " << Retry <<
                                      " I/O retries were necessary for reading " <<
                                      Vars[i].Name << " from: " << OpenFileName << "\n";

                            std::cerr.flush();
                        }
                    }
                }
            }

            TotalReadSize += ReadSize;

            uint64_t CRC = crc64_omp(Data, ReadSize);
      if (CRC != (uint64_t) -1) {
                ++NErrs[1];

                int Rank;
                #ifndef GENERICIO_NO_MPI
                MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
                #else
                Rank = 0;
                #endif

                // All ranks will do this and have a good time!
                string dn = "gio_crc_errors";
                mkdir(dn.c_str(), 0777);

                srand(time(0));
                int DumpNum = rand();
                stringstream ssd;
                ssd << dn << "/gio_crc_error_dump." << Rank << "." << DumpNum << ".bin";

                stringstream ss;
                ss << dn << "/gio_crc_error_log." << Rank << ".txt";

                ofstream ofs(ss.str().c_str(), ofstream::out | ofstream::app);
                ofs << "On-Disk CRC Error Report:\n";
                ofs << "Variable: " << Vars[i].Name << "\n";
                ofs << "File: " << OpenFileName << "\n";
                ofs << "I/O Retries: " << Retry << "\n"; 
                ofs << "Size: " << ReadSize << " bytes\n";
                ofs << "Offset: " << Offset << " bytes\n";
                ofs << "CRC: " << CRC << " (expected is -1)\n";
                ofs << "Dump file: " << ssd.str() << "\n";
                ofs << "\n";
                ofs.close();

                ofstream dofs(ssd.str().c_str(), ofstream::out);
                dofs.write((const char *) Data, ReadSize);
                dofs.close();

                uint64_t RawCRC = crc64_omp(Data, ReadSize - CRCSize);
                unsigned char *UData = (unsigned char *) Data;
                crc64_invert(RawCRC, &UData[ReadSize - CRCSize]);
                uint64_t NewCRC = crc64_omp(Data, ReadSize);
                std::cerr << "Recalulated CRC: " << NewCRC << ((NewCRC == -1) ? "ok" : "bad") << "\n";
                break;
            }

            if (HasExtraSpace)
                std::copy(CRCSave, CRCSave + CRCSize, CRCLoc);

      if (LData.size()) {
                CompressHeader<IsBigEndian> *CH = (CompressHeader<IsBigEndian>*) &LData[0];

                #ifdef _OPENMP
                #pragma omp master
                {
                #endif

       if (!blosc_initialized) {
                        blosc_init();
                        blosc_initialized = true;
                    }

                    #ifdef _OPENMP
                    blosc_set_nthreads(omp_get_max_threads());
                }
                    #endif

                blosc_decompress(&LData[0] + sizeof(CompressHeader<IsBigEndian>),
                                 VarData, Vars[i].Size * RH->NElems);

        if (CH->OrigCRC != crc64_omp(VarData, Vars[i].Size*RH->NElems)) {
                    ++NErrs[2];
                    break;
                }
            }

            // Byte swap the data if necessary.
            if (IsBigEndian != isBigEndian())
                for (size_t j = 0;
             j < RH->NElems*(Vars[i].Size/Vars[i].ElementSize); ++j) {
                    char *Offset = ((char *) VarData) + j * Vars[i].ElementSize;
                    bswap(Offset, Vars[i].ElementSize);
                }

            break;
        }

        if (!VarFound)
            throw runtime_error("Variable " + Vars[i].Name +
                                " not found in: " + OpenFileName);

        // This is for debugging.
    if (NErrs[0] || NErrs[1] || NErrs[2]) {
            const char *EnvStr = getenv("GENERICIO_VERBOSE");
      if (EnvStr) {
                int Mod = atoi(EnvStr);
        if (Mod > 0) {
                    int Rank;
                    #ifndef GENERICIO_NO_MPI
                    MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
                    #else
                    Rank = 0;
                    #endif

                    std::cerr << "Rank " << Rank << ": " << NErrs[0] << " I/O error(s), " <<
                              NErrs[1] << " CRC error(s) and " << NErrs[2] <<
                              " decompression CRC error(s) reading: " << Vars[i].Name <<
                              " from: " << OpenFileName << "\n";

                    std::cerr.flush();
                }
            }
        }

        if (NErrs[0] || NErrs[1] || NErrs[2])
            break;
    }
}



void GenericIO::readDataSection(size_t readOffset, size_t readNumRows, int EffRank, bool PrintStats, bool CollStats)
{
    int Rank;
  #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(Comm, &Rank);
  #else
    Rank = 0;
  #endif

    uint64_t TotalReadSize = 0;
  #ifndef GENERICIO_NO_MPI
    double StartTime = MPI_Wtime();
  #else
    double StartTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    int NErrs[3] = { 0, 0, 0 };

    if (EffRank == -1 && Redistributing)
    {
        DisableCollErrChecking = true;

        size_t RowOffset = 0;
        for (int i = 0, ie = SourceRanks.size(); i != ie; ++i)
        {
            readDataSection(readOffset, readNumRows, SourceRanks[i], RowOffset, Rank, TotalReadSize, NErrs);
            RowOffset += readNumElems(SourceRanks[i]);
        }

        DisableCollErrChecking = false;
    }
    else
    {
        readDataSection(readOffset, readNumRows, EffRank, 0, Rank, TotalReadSize, NErrs);
    }

    int AllNErrs[3];
  #ifndef GENERICIO_NO_MPI
    MPI_Allreduce(NErrs, AllNErrs, 3, MPI_INT, MPI_SUM, Comm);
  #else
    AllNErrs[0] = NErrs[0]; AllNErrs[1] = NErrs[1]; AllNErrs[2] = NErrs[2];
  #endif

    if (AllNErrs[0] > 0 || AllNErrs[1] > 0 || AllNErrs[2] > 0)
    {
        stringstream ss;
        ss << "Experienced " << AllNErrs[0] << " I/O error(s), " <<
           AllNErrs[1] << " CRC error(s) and " << AllNErrs[2] <<
           " decompression CRC error(s) reading: " << OpenFileName;
        throw runtime_error(ss.str());
    }

  #ifndef GENERICIO_NO_MPI
    MPI_Barrier(Comm);
  #endif

  #ifndef GENERICIO_NO_MPI
    double EndTime = MPI_Wtime();
  #else
    double EndTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    double TotalTime = EndTime - StartTime;
    double MaxTotalTime;
  #ifndef GENERICIO_NO_MPI
    if (CollStats)
        MPI_Reduce(&TotalTime, &MaxTotalTime, 1, MPI_DOUBLE, MPI_MAX, 0, Comm);
    else
  #endif
        MaxTotalTime = TotalTime;

    uint64_t AllTotalReadSize;
  #ifndef GENERICIO_NO_MPI
    if (CollStats)
        MPI_Reduce(&TotalReadSize, &AllTotalReadSize, 1, MPI_UINT64_T, MPI_SUM, 0, Comm);
    else
  #endif
        AllTotalReadSize = TotalReadSize;

    if (Rank == 0 && PrintStats)
    {
        double Rate = ((double) AllTotalReadSize) / MaxTotalTime / (1024.*1024.);
        cout << "Read " << Vars.size() << " variables from " << FileName <<
             " (" << AllTotalReadSize << " bytes) in " << MaxTotalTime << "s: " <<
             Rate << " MB/s [excluding header read]" << endl;
    }
}


void GenericIO::readDataSection(size_t readOffset, size_t readNumRows, int EffRank, size_t RowOffset, int Rank, uint64_t &TotalReadSize, int NErrs[3])
{
    if (FH.isBigEndian())
        readDataSection<true>(readOffset, readNumRows, EffRank, RowOffset, Rank, TotalReadSize, NErrs);
    else
        readDataSection<false>(readOffset, readNumRows, EffRank, RowOffset, Rank, TotalReadSize, NErrs);
}

template <bool IsBigEndian>
void GenericIO::readDataSection(size_t readOffset, size_t readNumRows, int EffRank, size_t RowOffset, int Rank, uint64_t &TotalReadSize, int NErrs[3])
{
    openAndReadHeader(Redistributing ? MismatchRedistribute : MismatchAllowed, EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    if (EffRank == -1)
        EffRank = Rank;

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];

    for (size_t i = 0; i < Vars.size(); ++i)
    {
        uint64_t Offset = RH->Start;
        bool VarFound = false;
        for (uint64_t j = 0; j < GH->NVars; ++j)
        {
            VariableHeader<IsBigEndian> *VH = (VariableHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->VarsStart + j * GH->VarsSize];

            string VName(VH->Name, VH->Name + NameSize);
            size_t VNameNull = VName.find('\0');
            if (VNameNull < NameSize)
                VName.resize(VNameNull);

            uint64_t ReadSize = RH->NElems * VH->Size + CRCSize;
            if (VName != Vars[i].Name)
            {
                Offset += ReadSize;
                continue;
            }

            VarFound = true;
            bool IsFloat = (bool) (VH->Flags & FloatValue), IsSigned = (bool) (VH->Flags & SignedValue);
            if (VH->Size != Vars[i].Size)
            {
                stringstream ss;
                ss << "Size mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " << Vars[i].Size <<
                   ", file: " << VH->Size;
                throw runtime_error(ss.str());
            }
            else if (IsFloat != Vars[i].IsFloat)
            {
                string Float("float"), Int("integer");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsFloat ? Float : Int) <<
                   ", file: " << (IsFloat ? Float : Int);
                throw runtime_error(ss.str());
            }
            else if (IsSigned != Vars[i].IsSigned)
            {
                string Signed("signed"), Uns("unsigned");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsSigned ? Signed : Uns) <<
                   ", file: " << (IsSigned ? Signed : Uns);
                throw runtime_error(ss.str());
            }

            size_t VarOffset = RowOffset * Vars[i].Size;
            void *VarData = ((char *) Vars[i].Data) + VarOffset;

            vector<unsigned char> LData;
            void *Data = VarData;
            bool HasExtraSpace = Vars[i].HasExtraSpace;
            if (offsetof_safe(GH, BlocksStart) < GH->GlobalHeaderSize && GH->BlocksSize > 0)
            {
                BlockHeader<IsBigEndian> *BH = (BlockHeader<IsBigEndian> *)
                                               &FH.getHeaderCache()[GH->BlocksStart +
                                                       (RankIndex * GH->NVars + j) * GH->BlocksSize];

                ReadSize = BH->Size + CRCSize;
                Offset = BH->Start;

                if (strncmp(BH->Filters[0], CompressName, FilterNameSize) == 0)
                {
                    LData.resize(ReadSize);
                    Data = &LData[0];
                    HasExtraSpace = true;
                }
                else if (BH->Filters[0][0] != '\0')
                {
                    stringstream ss;
                    ss << "Unknown filter \"" << BH->Filters[0] << "\" on variable " << Vars[i].Name;
                    throw runtime_error(ss.str());
                }
            }

            assert(HasExtraSpace && "Extra space required for reading");

            int Retry = 0;
            {
                int RetryCount = 300;
                const char *EnvStr = getenv("GENERICIO_RETRY_COUNT");
                if (EnvStr)
                    RetryCount = atoi(EnvStr);

                int RetrySleep = 100; // ms
                EnvStr = getenv("GENERICIO_RETRY_SLEEP");
                if (EnvStr)
                    RetrySleep = atoi(EnvStr);

                for (; Retry < RetryCount; ++Retry)
                {
                    try
                    {
                        //
                        // Read section
                        ReadSize = readNumRows * VH->Size;
                        Offset = Offset + readOffset * VH->Size;
                        FH.get()->read(Data, ReadSize, Offset, Vars[i].Name);

                        break;
                    }
                    catch (...) { }

                    usleep(1000 * RetrySleep);
                }

                if (Retry == RetryCount)
                {
                    ++NErrs[0];
                    break;
                }
                else if (Retry > 0)
                {
                    EnvStr = getenv("GENERICIO_VERBOSE");
                    if (EnvStr)
                    {
                        int Mod = atoi(EnvStr);
                        if (Mod > 0)
                        {
                            int Rank;
                          #ifndef GENERICIO_NO_MPI
                            MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
                          #else
                            Rank = 0;
                          #endif

                            std::cerr << "Rank " << Rank << ": " << Retry <<
                                      " I/O retries were necessary for reading " <<
                                      Vars[i].Name << " from: " << OpenFileName << "\n";

                            std::cerr.flush();
                        }
                    }
                }
            }

            TotalReadSize += ReadSize;

            // Byte swap the data if necessary.
            if (IsBigEndian != isBigEndian())
                for (size_t j = 0; j < RH->NElems; ++j)
                {
                    char *Offset = ((char *) VarData) + j * Vars[i].Size;
                    bswap(Offset, Vars[i].Size);
                }

            break;
        }

        if (!VarFound)
            throw runtime_error("Variable " + Vars[i].Name + " not found in: " + OpenFileName);
    }
}




void GenericIO::readDataSectionNoMPIBarrier(size_t readOffset, size_t readNumRows, int EffRank, bool PrintStats, bool CollStats)
{
    int Rank;
  #ifndef GENERICIO_NO_MPI
    MPI_Comm_rank(Comm, &Rank);
  #else
    Rank = 0;
  #endif

    uint64_t TotalReadSize = 0;
  #ifndef GENERICIO_NO_MPI
    double StartTime = MPI_Wtime();
  #else
    double StartTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    int NErrs[3] = { 0, 0, 0 };

    if (EffRank == -1 && Redistributing)
    {
        DisableCollErrChecking = true;

        size_t RowOffset = 0;
        for (int i = 0, ie = SourceRanks.size(); i != ie; ++i)
        {
            readDataSection(readOffset, readNumRows, SourceRanks[i], RowOffset, Rank, TotalReadSize, NErrs);
            RowOffset += readNumElems(SourceRanks[i]);
        }

        DisableCollErrChecking = false;
    }
    else
    {
        readDataSection(readOffset, readNumRows, EffRank, 0, Rank, TotalReadSize, NErrs);
    }

    int AllNErrs[3];
    AllNErrs[0] = NErrs[0]; AllNErrs[1] = NErrs[1]; AllNErrs[2] = NErrs[2];

    if (AllNErrs[0] > 0 || AllNErrs[1] > 0 || AllNErrs[2] > 0)
    {
        stringstream ss;
        ss << "Experienced " << AllNErrs[0] << " I/O error(s), " <<
           AllNErrs[1] << " CRC error(s) and " << AllNErrs[2] <<
           " decompression CRC error(s) reading: " << OpenFileName;
        throw runtime_error(ss.str());
    }

  #ifndef GENERICIO_NO_MPI
    double EndTime = MPI_Wtime();
  #else
    double EndTime = double(clock()) / CLOCKS_PER_SEC;
  #endif

    double TotalTime = EndTime - StartTime;
    double MaxTotalTime;
    MaxTotalTime = TotalTime;

    uint64_t AllTotalReadSize;
    AllTotalReadSize = TotalReadSize;

    if (Rank == 0 && PrintStats)
    {
        double Rate = ((double) AllTotalReadSize) / MaxTotalTime / (1024.*1024.);
        cout << "Read " << Vars.size() << " variables from " << FileName <<
             " (" << AllTotalReadSize << " bytes) in " << MaxTotalTime << "s: " <<
             Rate << " MB/s [excluding header read]" << endl;
    }
}


void GenericIO::readDataSectionNoMPIBarrier(size_t readOffset, size_t readNumRows, int EffRank, size_t RowOffset, int Rank, uint64_t &TotalReadSize, int NErrs[3])
{
    if (FH.isBigEndian())
        readDataSectionNoMPIBarrier<true>(readOffset, readNumRows, EffRank, RowOffset, Rank, TotalReadSize, NErrs);
    else
        readDataSectionNoMPIBarrier<false>(readOffset, readNumRows, EffRank, RowOffset, Rank, TotalReadSize, NErrs);
}


template <bool IsBigEndian>
void GenericIO::readDataSectionNoMPIBarrier(size_t readOffset, size_t readNumRows, int EffRank, size_t RowOffset, int Rank, uint64_t &TotalReadSize, int NErrs[3])
{
    openAndReadHeader(Redistributing ? MismatchRedistribute : MismatchAllowed, EffRank, false);

    assert(FH.getHeaderCache().size() && "HeaderCache must not be empty");

    if (EffRank == -1)
        EffRank = Rank;

    GlobalHeader<IsBigEndian> *GH = (GlobalHeader<IsBigEndian> *) &FH.getHeaderCache()[0];
    size_t RankIndex = getRankIndex<IsBigEndian>(EffRank, GH, RankMap, FH.getHeaderCache());

    assert(RankIndex < GH->NRanks && "Invalid rank specified");

    RankHeader<IsBigEndian> *RH = (RankHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->RanksStart +
                                  RankIndex * GH->RanksSize];

    for (size_t i = 0; i < Vars.size(); ++i)
    {
        uint64_t Offset = RH->Start;
        bool VarFound = false;
        for (uint64_t j = 0; j < GH->NVars; ++j)
        {
            VariableHeader<IsBigEndian> *VH = (VariableHeader<IsBigEndian> *) &FH.getHeaderCache()[GH->VarsStart + j * GH->VarsSize];

            string VName(VH->Name, VH->Name + NameSize);
            size_t VNameNull = VName.find('\0');
            if (VNameNull < NameSize)
                VName.resize(VNameNull);

            uint64_t ReadSize = RH->NElems * VH->Size + CRCSize;
            if (VName != Vars[i].Name)
            {
                Offset += ReadSize;
                continue;
            }

            VarFound = true;
            bool IsFloat = (bool) (VH->Flags & FloatValue), IsSigned = (bool) (VH->Flags & SignedValue);
            if (VH->Size != Vars[i].Size)
            {
                stringstream ss;
                ss << "Size mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " << Vars[i].Size <<
                   ", file: " << VH->Size;
                throw runtime_error(ss.str());
            }
            else if (IsFloat != Vars[i].IsFloat)
            {
                string Float("float"), Int("integer");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsFloat ? Float : Int) <<
                   ", file: " << (IsFloat ? Float : Int);
                throw runtime_error(ss.str());
            }
            else if (IsSigned != Vars[i].IsSigned)
            {
                string Signed("signed"), Uns("unsigned");
                stringstream ss;
                ss << "Type mismatch for variable " << Vars[i].Name <<
                   " in: " << OpenFileName << ": current: " <<
                   (Vars[i].IsSigned ? Signed : Uns) <<
                   ", file: " << (IsSigned ? Signed : Uns);
                throw runtime_error(ss.str());
            }

            size_t VarOffset = RowOffset * Vars[i].Size;
            void *VarData = ((char *) Vars[i].Data) + VarOffset;

            vector<unsigned char> LData;
            void *Data = VarData;
            bool HasExtraSpace = Vars[i].HasExtraSpace;
            if (offsetof_safe(GH, BlocksStart) < GH->GlobalHeaderSize && GH->BlocksSize > 0)
            {
                BlockHeader<IsBigEndian> *BH = (BlockHeader<IsBigEndian> *)
                                               &FH.getHeaderCache()[GH->BlocksStart +
                                                       (RankIndex * GH->NVars + j) * GH->BlocksSize];

                ReadSize = BH->Size + CRCSize;
                Offset = BH->Start;

                if (strncmp(BH->Filters[0], CompressName, FilterNameSize) == 0)
                {
                    LData.resize(ReadSize);
                    Data = &LData[0];
                    HasExtraSpace = true;
                }
                else if (BH->Filters[0][0] != '\0')
                {
                    stringstream ss;
                    ss << "Unknown filter \"" << BH->Filters[0] << "\" on variable " << Vars[i].Name;
                    throw runtime_error(ss.str());
                }
            }

            assert(HasExtraSpace && "Extra space required for reading");

            int Retry = 0;
            {
                int RetryCount = 300;
                const char *EnvStr = getenv("GENERICIO_RETRY_COUNT");
                if (EnvStr)
                    RetryCount = atoi(EnvStr);

                int RetrySleep = 100; // ms
                EnvStr = getenv("GENERICIO_RETRY_SLEEP");
                if (EnvStr)
                    RetrySleep = atoi(EnvStr);

                for (; Retry < RetryCount; ++Retry)
                {
                    try
                    {
                        //
                        // Read section
                        ReadSize = readNumRows * VH->Size;
                        Offset = Offset + readOffset * VH->Size;
                        FH.get()->read(Data, ReadSize, Offset, Vars[i].Name);

                        break;
                    }
                    catch (...) { }

                    usleep(1000 * RetrySleep);
                }

                if (Retry == RetryCount)
                {
                    ++NErrs[0];
                    break;
                }
                else if (Retry > 0)
                {
                    EnvStr = getenv("GENERICIO_VERBOSE");
                    if (EnvStr)
                    {
                        int Mod = atoi(EnvStr);
                        if (Mod > 0)
                        {
                            int Rank;
                            #ifndef GENERICIO_NO_MPI
                            MPI_Comm_rank(MPI_COMM_WORLD, &Rank);
                            #else
                            Rank = 0;
                            #endif

                            std::cerr << "Rank " << Rank << ": " << Retry <<
                                      " I/O retries were necessary for reading " <<
                                      Vars[i].Name << " from: " << OpenFileName << "\n";

                            std::cerr.flush();
                        }
                    }
                }
            }

            TotalReadSize += ReadSize;

            // Byte swap the data if necessary.
            if (IsBigEndian != isBigEndian())
                for (size_t j = 0; j < RH->NElems; ++j)
                {
                    char *Offset = ((char *) VarData) + j * Vars[i].Size;
                    bswap(Offset, Vars[i].Size);
                }

            break;
        }

        if (!VarFound)
            throw runtime_error("Variable " + Vars[i].Name + " not found in: " + OpenFileName);
    }
}

} /* END namespace cosmotk */
