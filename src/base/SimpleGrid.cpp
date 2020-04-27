///////////////////////////////////////////////////////////////////////////////
///
///	\file    SimpleGrid.cpp
///	\author  Paul Ullrich
///	\version August 30, 2019
///
///	<remarks>
///		Copyright 2019 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "SimpleGrid.h"
#include "GridElements.h"
#include "FiniteElementTools.h"
#include "GaussLobattoQuadrature.h"
#include "Announce.h"
#include "kdtree.h"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>

#include "netcdfcpp.h"

///////////////////////////////////////////////////////////////////////////////

const char * SimpleGrid::c_szFileIdentifier =
	"#TempestGridConnectivityFileV2.0";

///////////////////////////////////////////////////////////////////////////////

SimpleGrid::~SimpleGrid() {
	if (m_kdtree != NULL) {
		kd_free(m_kdtree);
	}
}

///////////////////////////////////////////////////////////////////////////////

bool SimpleGrid::IsInitialized() const {
	if ((m_nGridDim.size() != 0) ||
	    m_dLon.IsAttached() ||
	    m_dLat.IsAttached() ||
	    m_dArea.IsAttached() ||
	    (m_vecConnectivity.size() != 0) ||
		(m_kdtree != NULL)
	) {
		return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::GenerateLatitudeLongitude(
	const DataArray1D<double> & vecLat,
	const DataArray1D<double> & vecLon,
	bool fRegional
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call GenerateLatitudeLongitude() on previously initialized grid");
	}

	int nLat = vecLat.GetRows();
	int nLon = vecLon.GetRows();

	if (nLat < 2) {
		_EXCEPTIONT("At least two latitudes needed to generate grid.");
	}
	if (nLon < 2) {
		_EXCEPTIONT("At least two longitudes needed to generate grid.");
	}

	m_dLat.Allocate(nLon * nLat);
	m_dLon.Allocate(nLon * nLat);
	m_dArea.Allocate(nLon * nLat);
	m_vecConnectivity.resize(nLon * nLat);

	m_nGridDim.resize(2);
	m_nGridDim[0] = nLat;
	m_nGridDim[1] = nLon;

	// Verify units of latitude and longitude
	for (int j = 0; j < nLat; j++) {
		if (fabs(vecLat[j]) > 0.5 * M_PI + 1.0e-12) {
			_EXCEPTIONT("Latitude array must be given in radians");
		}
	}
	for (int i = 0; i < nLon; i++) {
		if (fabs(vecLon[i]) > 2.0 * M_PI + 1.0e-12) {
			_EXCEPTIONT("Longitude array must be given in radians");
		}
	}

	// Determine orientation of latitude array
	double dLatOrient = 1.0;
	if (vecLat[1] < vecLat[0]) {
		dLatOrient = -1.0;
	}
	for (int j = 0; j < nLat-1; j++) {
		if (dLatOrient * vecLat[1] < dLatOrient * vecLat[0]) {
			_EXCEPTIONT("Latitude array must be monotone.");
		}
	}

	int ixs = 0;
	for (int j = 0; j < nLat; j++) {
	for (int i = 0; i < nLon; i++) {

		// Vectorize coordinates
		m_dLat[ixs] = vecLat[j];
		m_dLon[ixs] = vecLon[i];

		// Bounds of each volume and associated area
		double dLatRad1;
		double dLatRad2;
		
		double dLonRad1;
		double dLonRad2;

		if (j == 0) {
			if (fRegional) {
				dLatRad1 = vecLat[0] - 0.5 * (vecLat[1] - vecLat[0]);
			} else {
				dLatRad1 = - dLatOrient * 0.5 * M_PI;
			}
		} else {
			dLatRad1 = 0.5 * (vecLat[j-1] + vecLat[j]);
		}
		if (j == nLat-1) {
			if (fRegional) {
				dLatRad2 = vecLat[j] + 0.5 * (vecLat[j] - vecLat[j-1]);
			} else {
				dLatRad2 = dLatOrient * 0.5 * M_PI;
			}
		} else {
			dLatRad2 = 0.5 * (vecLat[j+1] + vecLat[j]);
		}

		if (i == 0) {
			if (fRegional) {
				dLonRad1 = vecLon[0] - 0.5 * (vecLon[1] - vecLon[0]);
			} else {
				dLonRad1 = AverageLongitude_Rad(vecLon[0], vecLon[nLon-1]);
			}
		} else {
			dLonRad1 = AverageLongitude_Rad(vecLon[i-1], vecLon[i]);
		}

		if (i == nLon-1) {
			if (fRegional) {
				dLonRad2 = vecLon[i] + 0.5 * (vecLon[i] - vecLon[i-1]);
			} else {
				dLonRad2 = AverageLongitude_Rad(vecLon[nLon-1], vecLon[0]);
			}
		} else {
			dLonRad2 = AverageLongitude_Rad(vecLon[i], vecLon[i+1]);
		}

		// Because of the way AverageLongitude_Rad works,
		if (dLonRad1 > dLonRad2) {
			dLonRad1 -= 2.0 * M_PI;
		}
		double dDeltaLon = dLonRad2 - dLonRad1;
		if (dDeltaLon >= M_PI) {
			_EXCEPTION1("Grid element longitudinal extent too large (%1.7f deg).  Did you mean to specify \"--regional\"?",
				dDeltaLon * 180.0 / M_PI);
		}

		m_dArea[ixs] = fabs(sin(dLatRad2) - sin(dLatRad1)) * dDeltaLon;

		// Connectivity in each compass direction
		if (j != 0) {
			m_vecConnectivity[ixs].push_back((j-1) * nLon + i);
		}
		if (j != nLat-1) {
			m_vecConnectivity[ixs].push_back((j+1) * nLon + i);
		}

		if ((!fRegional) ||
		    ((i != 0) && (i != nLon-1))
		) {
			m_vecConnectivity[ixs].push_back(
				j * nLon + ((i + 1) % nLon));
			m_vecConnectivity[ixs].push_back(
				j * nLon + ((i + nLon - 1) % nLon));
		}

		ixs++;
	}
	}

	// Output total area
	{
		double dTotalArea = 0.0;
		for (size_t i = 0; i < m_dArea.GetRows(); i++) {
			dTotalArea += m_dArea[i];
		}
		Announce("Total calculated area: %1.15e", dTotalArea);
	}

}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::GenerateLatitudeLongitude(
	NcFile * ncFile,
	bool fRegional,
	const std::string & strLatitudeName,
	const std::string & strLongitudeName
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call GenerateLatitudeLongitude() on previously initialized grid");
	}

	NcDim * dimLat = ncFile->get_dim(strLatitudeName.c_str());
	if (dimLat == NULL) {
		_EXCEPTION1("No dimension \"%s\" found in input file",
			strLatitudeName.c_str());
	}

	NcDim * dimLon = ncFile->get_dim(strLongitudeName.c_str());
	if (dimLon == NULL) {
		_EXCEPTION1("No dimension \"%s\" found in input file",
			strLongitudeName.c_str());
	}

	NcVar * varLat = ncFile->get_var(strLatitudeName.c_str());
	if (varLat == NULL) {
		_EXCEPTION1("No variable \"%s\" found in input file",
			strLatitudeName.c_str());
	}

	NcVar * varLon = ncFile->get_var(strLongitudeName.c_str());
	if (varLon == NULL) {
		_EXCEPTION1("No variable \"%s\" found in input file",
			strLongitudeName.c_str());
	}

	int nLat = dimLat->size();
	int nLon = dimLon->size();

	DataArray1D<double> vecLat(nLat);
	varLat->get(vecLat, nLat);

	for (int j = 0; j < nLat; j++) {
		vecLat[j] *= M_PI / 180.0;
	}

	DataArray1D<double> vecLon(nLon);
	varLon->get(vecLon, nLon);

	for (int i = 0; i < nLon; i++) {
		vecLon[i] *= M_PI / 180.0;
	}

	// Generate the SimpleGrid
	GenerateLatitudeLongitude(vecLat, vecLon, fRegional);
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::GenerateLatitudeLongitude(
	NcFile * ncFile,
	bool fRegional
) {
	return GenerateLatitudeLongitude(ncFile, fRegional, "lat", "lon");
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::GenerateRectilinearStereographic(
	double dLonRad0,
	double dLatRad0,
	int nX,
	double dDeltaXDeg,
	bool fCalculateArea
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call GenerateRectilinearStereographic() on previously initialized grid");
	}

	_ASSERT(nX >= 1);
	_ASSERT(dDeltaXDeg > 0.0);
	_ASSERT(fabs(dLatRad0 <= 0.5 * M_PI));

	if (fabs(dLatRad0 - 0.5 * M_PI) < ReferenceTolerance) {
		dLatRad0 = 0.5 * M_PI;
	}
	if (fabs(dLatRad0 + 0.5 * M_PI) < ReferenceTolerance) {
		dLatRad0 = - 0.5 * M_PI;
	}

	double dDeltaXRad = M_PI / 180.0 * dDeltaXDeg;

	m_nGridDim.resize(2);
	m_nGridDim[0] = nX;
	m_nGridDim[1] = nX;

	m_dLon.Allocate(nX * nX);
	m_dLat.Allocate(nX * nX);

	const double dXgcd0 = - 0.5 * dDeltaXRad * static_cast<double>(nX - 1);

	if (dXgcd0 < - M_PI + ReferenceTolerance) {
		_EXCEPTION1("Total angular coverage of rectilinear stereographic "
			"grid too large (%1.5f <= -pi/2)", dXgcd0);
	}

	// Get coordinates in the space of the stereographic projection
	DataArray1D<double> dXs(nX);
	DataArray1D<double> dYs(nX);

	for (int j = 0; j < nX; j++) {
		double dYgcd = dXgcd0 + dDeltaXRad * static_cast<double>(j);
		dYs[j] = sqrt(4.0 * (1.0 - cos(dYgcd)) / (1.0 + cos(dYgcd)));
		if (dYgcd < 0.0) {
			dYs[j] *= -1.0;
		}
	}

	for (int i = 0; i < nX; i++) {
		double dXgcd = dXgcd0 + dDeltaXRad * static_cast<double>(i);
		dXs[i] = sqrt(4.0 * (1.0 - cos(dXgcd)) / (1.0 + cos(dXgcd)));
		if (dXgcd < 0.0) {
			dXs[i] *= -1.0;
		}
	}

	// Store longitude and latitude of centerpoints
	int s = 0;
	for (int j = 0; j < nX; j++) {
	for (int i = 0; i < nX; i++) {
		StereographicProjectionInv(
			dLonRad0,
			dLatRad0,
			dXs[i],
			dYs[j],
			m_dLon[s],
			m_dLat[s]);
/*
		// DEBUG: Verify that the great circle distance is correct
		printf("%i %i %1.5f\n", i, j,
			180.0 / M_PI
			* GreatCircleDistance_Rad(
				dLonRad0,
				dLatRad0,
				m_dLon[s],
				m_dLat[s]));
*/
		s++;
	}
	}

	// Calculate the area
	if (fCalculateArea) {
		_EXCEPTIONT("Unable to calculate the area of the RectilinearStereographic grid (not impemented)");
	}
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::GenerateRadialStereographic(
	double dLonRad0,
	double dLatRad0,
	int nR,
	int nA,
	double dDeltaRDeg,
	bool fCalculateArea
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call GenerateRadialStereographic() on previously initialized grid");
	}

	if (nA < 8) {
		_EXCEPTIONT("Minimum of 8 azimuthal slices allowed");
	}

	_ASSERT(nR >= 1);
	_ASSERT(dDeltaRDeg > 0.0);
	_ASSERT(fabs(dLatRad0 <= 0.5 * M_PI));

	double dDeltaRRad = M_PI / 180.0 * dDeltaRDeg;

	const double dRgcdmax = (static_cast<double>(nR-1) + 0.5) * dDeltaRRad;

	if (dRgcdmax >= M_PI) {
		_EXCEPTION1("Total angular coverage of radial stereographic "
			"grid too large (%1.5f >= pi)", dRgcdmax);
	}

	m_nGridDim.resize(2);
	m_nGridDim[0] = nA;
	m_nGridDim[1] = nR;

	m_dLon.Allocate(nA * nR);
	m_dLat.Allocate(nA * nR);

	// Get the reference coordinates in the azimuthal and radial directions
	DataArray1D<double> dXs(nA);
	DataArray1D<double> dYs(nA);

	for (int i = 0; i < nA; i++) {
		double dAzimuth = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(nA);
		dXs[i] = cos(dAzimuth);
		dYs[i] = sin(dAzimuth);
	}

	DataArray1D<double> dRs(nR);
	for (int i = 0; i < nR; i++) {
		double dRgcd = (static_cast<double>(i) + 0.5) * dDeltaRRad;
		dRs[i] = 2.0 * sqrt((1.0 - cos(dRgcd)) / (1.0 + cos(dRgcd)));
	}

	// Calculate the lon/lat coordinates
	int s = 0;
	for (int j = 0; j < nR; j++) {
	for (int i = 0; i < nA; i++) {
		StereographicProjectionInv(
			dLonRad0,
			dLatRad0,
			dXs[i] * dRs[j],
			dYs[i] * dRs[j],
			m_dLon[s],
			m_dLat[s]);
/*
		// DEBUG: Verify that the great circle distance is uniform
		// in the radial direction
		printf("%i %i %1.5f %1.5f %1.5f\n", i, j,
			dXs[i] * dRs[j],
			dYs[i] * dRs[j],
			180.0 / M_PI
			* GreatCircleDistance_Rad(
				dLonRad0,
				dLatRad0,
				m_dLon[s],
				m_dLat[s]));
*/
		s++;
	}
	}

	// Calculate the area
	if (fCalculateArea) {
		_EXCEPTIONT("Unable to calculate the area of the RectilinearStereographic grid (not impemented)");
	}

}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::FromMeshFV(
	const Mesh & mesh
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call FromMeshFV() on previously initialized grid");
	}

	if (mesh.vecFaceArea.GetRows() == 0) {
		_EXCEPTIONT("Mesh::CalculateFaceAreas() must be called prior "
			"to SimpleGrid::FromMeshFV()");
	}
	if (mesh.edgemap.size() == 0) {
		_EXCEPTIONT("Mesh::ConstructEdgeMap() must be called prior "
			"to SimpleGrid::FromMeshFV()");
	}

	// Number of faces
	size_t sFaces = mesh.faces.size();

	// Copy over areas
	m_dArea = mesh.vecFaceArea;

	// Generate connectivity from edge map
	std::vector< std::set<int> > m_vecConnectivitySet;
	m_vecConnectivitySet.resize(sFaces);

	EdgeMapConstIterator iterEdgeMap = mesh.edgemap.begin();
	for (; iterEdgeMap != mesh.edgemap.end(); iterEdgeMap++) {
		const FacePair & facepr = iterEdgeMap->second;
		if ((facepr[0] < 0) || (facepr[0] >= sFaces)) {
			_EXCEPTION1("EdgeMap FacePair out of range (%i)", facepr[0]);
		}
		if ((facepr[1] < 0) || (facepr[1] >= sFaces)) {
			_EXCEPTION1("EdgeMap FacePair out of range (%i)", facepr[1]);
		}
		m_vecConnectivitySet[facepr[0]].insert(facepr[1]);
		m_vecConnectivitySet[facepr[1]].insert(facepr[0]);
	}

	m_vecConnectivity.resize(sFaces);
	for (int i = 0; i < sFaces; i++) {
		std::set<int>::const_iterator iterConnect =
			m_vecConnectivitySet[i].begin();
		for (; iterConnect != m_vecConnectivitySet[i].end(); iterConnect++) {
			m_vecConnectivity[i].push_back(*iterConnect);
		}
	}

	// Generate centerpoints
	m_dLon.Allocate(sFaces);
	m_dLat.Allocate(sFaces);

	// Mesh is a RLL mesh -- special calculation for centerpoint values
	if (mesh.type == Mesh::MeshType_RLL) {
		_EXCEPTIONT("How to define nGridDim?");

		for (int i = 0; i < sFaces; i++) {

			const Face & face = mesh.faces[i];

			int nNodes = face.edges.size();

			if (nNodes != 4) {
				_EXCEPTIONT("RLL mesh must have exactly 4 nodes per face");
			}

			double dLonc = 0.0;
			double dLatc = 0.0;

			for (int j = 0; j < nNodes; j++) {

				const Node & node = mesh.nodes[face[j]];

				double dLon;
				double dLat;

				XYZtoRLL_Deg(
					node.x,
					node.y,
					node.z,
					dLon,
					dLat);

				// Consider periodicity of longitude
				if ((j != 0) && (fabs(dLonc / static_cast<double>(j) - dLon) > 180.0)) {
					if (dLonc > dLon) {
						dLon += 360.0;
					}
					if (dLonc < dLon) {
						dLon -= 360.0;
					}
				}

				// Sanity check: Longitude still out of range
				if ((j != 0) && (fabs(dLonc / static_cast<double>(j) - dLon) > 180.0)) {
					printf("FATAL:  Mesh face appears to extend more than 180 degrees longitude\n");
					for (int k = 0; k < j; k++) {

						const Node & nodeK = mesh.nodes[face[k]];

						XYZtoRLL_Deg(
							nodeK.x,
							nodeK.y,
							nodeK.z,
							dLon,
							dLat);

						printf("Node %i: %1.15e %1.15e\n", k, dLon, dLat);
					}
					_EXCEPTION();
				}

				dLonc += dLon;
				dLatc += dLat;
			}

			m_dLon[i] = dLonc / static_cast<double>(nNodes);
			m_dLat[i] = dLatc / static_cast<double>(nNodes);
		}

	// Any other kind of mesh, use Carteisan coords and project to surface of sphere
	} else {
		m_nGridDim.resize(1);
		m_nGridDim[0] = sFaces;

		for (size_t i = 0; i < sFaces; i++) {

			const Face & face = mesh.faces[i];

			int nNodes = face.edges.size();

			double dXc = 0.0;
			double dYc = 0.0;
			double dZc = 0.0;

			for (int j = 0; j < nNodes; j++) {
				const Node & node = mesh.nodes[face[j]];

				dXc += node.x;
				dYc += node.y;
				dZc += node.z;
			}

			dXc /= static_cast<double>(nNodes);
			dYc /= static_cast<double>(nNodes);
			dZc /= static_cast<double>(nNodes);

			XYZtoRLL_Deg(dXc, dYc, dZc, m_dLon[i], m_dLat[i]);
		}
	}

	// Output total area
	{
		double dTotalArea = 0.0;
		for (size_t i = 0; i < sFaces; i++) {
			dTotalArea += m_dArea[i];
		}
		Announce("Total calculated area: %1.15e", dTotalArea);
	}
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::FromMeshFE(
	const Mesh & mesh,
	bool fCGLL,
	int nP
) {
	if (!fCGLL) {
		_EXCEPTIONT("Sorry, not implemented yet!");
	}

	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call FromMeshFE() on previously initialized grid");
	}

	if (mesh.vecFaceArea.GetRows() == 0) {
		_EXCEPTIONT("Mesh::CalculateFaceAreas() must be called prior "
			"to SimpleGrid::FromMeshFE()");
	}
	if (mesh.edgemap.size() == 0) {
		_EXCEPTIONT("Mesh::ConstructEdgeMap() must be called prior "
			"to SimpleGrid::FromMeshFE()");
	}

	// Number of elements
	size_t nElements = mesh.faces.size();

	// Gauss-Lobatto nodes and weights
	DataArray1D<double> dG(nP);
	DataArray1D<double> dW(nP);

	GaussLobattoQuadrature::GetPoints(nP, 0.0, 1.0, dG, dW);

	// Generate coincident node map and Jacobian
	DataArray3D<int> dataGLLnodes(nP, nP, nElements);
	DataArray3D<double> dataGLLJacobian(nP, nP, nElements);

	GenerateMetaData(mesh, nP, true, dataGLLnodes, dataGLLJacobian);

	// Generate areas
	if (fCGLL) {
		GenerateUniqueJacobian(
			dataGLLnodes,
			dataGLLJacobian,
			m_dArea);
	} else {
		GenerateDiscontinuousJacobian(
			dataGLLJacobian,
			m_dArea);
	}

	// Number of faces
	size_t sFaces = m_dArea.GetRows();

	m_nGridDim.resize(1);
	m_nGridDim[0] = sFaces;

	// Generate coordinates
	{
		m_dLon.Allocate(sFaces);
		m_dLat.Allocate(sFaces);

		const NodeVector & nodevec = mesh.nodes;
		for (int k = 0; k < nElements; k++) {
			const Face & face = mesh.faces[k];

			if (face.edges.size() != 4) {
				_EXCEPTIONT("Mesh must only contain quadrilateral elements");
			}

			double dFaceNumericalArea = 0.0;

			for (int j = 0; j < nP; j++) {
			for (int i = 0; i < nP; i++) {
				int ix = dataGLLnodes[j][i][k]-1;

				_ASSERT((ix >= 0) && (ix < m_dLon.GetRows()));

				Node nodeGLL;
				Node dDx1G;
				Node dDx2G;

				ApplyLocalMap(
					face,
					nodevec,
					dG[i],
					dG[j],
					nodeGLL,
					dDx1G,
					dDx2G);

				XYZtoRLL_Deg(
					nodeGLL.x,
					nodeGLL.y,
					nodeGLL.z,
					m_dLon[ix],
					m_dLat[ix]);
			}
			}
		}
	}

	// Generate connectivity
	{
		std::vector< std::set<int> > vecConnectivitySet;
		vecConnectivitySet.resize(sFaces);

		for (size_t f = 0; f < nElements; f++) {

			for (int q = 0; q < nP; q++) {
			for (int p = 0; p < nP; p++) {

				std::set<int> & setLocalConnectivity =
					vecConnectivitySet[dataGLLnodes[q][p][f]-1];

				// Connect in all directions
				if (p != 0) {
					setLocalConnectivity.insert(
						dataGLLnodes[q][p-1][f]);
				}
				if (p != (nP-1)) {
					setLocalConnectivity.insert(
						dataGLLnodes[q][p+1][f]);
				}
				if (q != 0) {
					setLocalConnectivity.insert(
						dataGLLnodes[q-1][p][f]);
				}
				if (q != (nP-1)) {
					setLocalConnectivity.insert(
						dataGLLnodes[q+1][p][f]);
				}
			}
			}
		}

		m_vecConnectivity.resize(sFaces);
		for (size_t i = 0; i < sFaces; i++) {
			std::set<int>::const_iterator iterConnect =
				vecConnectivitySet[i].begin();
			for (; iterConnect != vecConnectivitySet[i].end(); iterConnect++) {
				m_vecConnectivity[i].push_back(*iterConnect);
			}
		}
	}

	// Output total area
	{
		double dTotalArea = 0.0;
		for (size_t i = 0; i < sFaces; i++) {
			dTotalArea += m_dArea[i];
		}
		Announce("Total calculated area: %1.15e", dTotalArea);
	}
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::FromFile(
	const std::string & strConnectivityFile
) {
	if (IsInitialized()) {
		_EXCEPTIONT("Attempting to call FromFile() on previously initialized grid");
	}

	std::ifstream fsGrid(strConnectivityFile.c_str());
	if (!fsGrid.is_open()) {
		_EXCEPTION1("Unable to open file \"%s\"",
			strConnectivityFile.c_str());
	}

	std::string strConnectivityString;
	fsGrid >> strConnectivityString;

	if (strConnectivityString != c_szFileIdentifier) {
		_EXCEPTION1("Invalid connectivity file format \"%s\"",
			strConnectivityFile.c_str());
	}

	size_t sDims;
	fsGrid >> sDims;

	size_t sFaces = 1;

	if ((sDims < 1) || (sDims > 2)) {
		_EXCEPTION1("Invalid connectivity file: %lu dimensions out "
			"of range (expected 1,2)", sDims);
	}

	m_nGridDim.resize(sDims);
	for (size_t s = 0; s < sDims; s++) {
		char cComma;
		fsGrid >> cComma;
		fsGrid >> m_nGridDim[s];

		if (m_nGridDim[s] < 1) {
			_EXCEPTION2("Grid dimension %lu out of range (%lu found)",
				s, m_nGridDim[s]);
		}

		sFaces *= m_nGridDim[s];
	}

	m_dLon.Allocate(sFaces);
	m_dLat.Allocate(sFaces);
	m_dArea.Allocate(sFaces);
	m_vecConnectivity.resize(sFaces);

	for (size_t f = 0; f < sFaces; f++) {
		size_t sNeighbors;
		char cComma;
		fsGrid >> m_dLon[f];
		fsGrid >> cComma;
		fsGrid >> m_dLat[f];
		fsGrid >> cComma;
		fsGrid >> m_dArea[f];
		fsGrid >> cComma;
		fsGrid >> sNeighbors;
		fsGrid >> cComma;

		// Convert to radians
		m_dLon[f] *= M_PI / 180.0;
		m_dLat[f] *= M_PI / 180.0;

		// Load connectivity
		m_vecConnectivity[f].resize(sNeighbors);
		for (size_t n = 0; n < sNeighbors; n++) {
			fsGrid >> m_vecConnectivity[f][n];
			if (n != sNeighbors-1) {
				fsGrid >> cComma;
			}
			m_vecConnectivity[f][n]--;
		}
		if (fsGrid.eof()) {
			if (f != sFaces-1) {
				_EXCEPTIONT("Premature end of file");
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::ToFile(
	const std::string & strConnectivityFile
) const {
	std::ofstream fsOutput(strConnectivityFile.c_str());
	if (!fsOutput.is_open()) {
		_EXCEPTION1("Cannot open output file \"%s\"",
			strConnectivityFile.c_str());
	}

	fsOutput << std::setprecision(14);
	fsOutput << std::scientific;

	fsOutput << c_szFileIdentifier << std::endl;
	
	size_t sFaces = 1;
	fsOutput << m_nGridDim.size();
	for (size_t i = 0; i < m_nGridDim.size(); i++) {
		sFaces *= m_nGridDim[i];
		fsOutput << "," << m_nGridDim[i];
	}
	fsOutput << std::endl;

	if (m_dLon.GetRows() != sFaces) {
		_EXCEPTIONT("Mangled SimpleGrid structure: m_dLon.size() != size from m_nGridDim");
	}
	if (sFaces != m_dLat.GetRows()) {
		_EXCEPTIONT("Mangled SimpleGrid structure: m_dLon.size() != m_dLat.size()");
	}
	if (sFaces != m_dArea.GetRows()) {
		_EXCEPTIONT("Mangled SimpleGrid structure: m_dLon.size() != m_dArea.size()");
	}
	if (sFaces != m_vecConnectivity.size()) {
		_EXCEPTIONT("Mangled SimpleGrid structure: m_dLon.size() != m_vecConnectivity.size()");
	}

	for (size_t i = 0; i < sFaces; i++) {
		fsOutput << m_dLon[i] << "," << m_dLat[i] << ","
			<< m_dArea[i] << "," << m_vecConnectivity[i].size();
		for (size_t j = 0; j < m_vecConnectivity[i].size(); j++) {
			fsOutput << "," << m_vecConnectivity[i][j];
		}
		fsOutput << std::endl;
	}
}

///////////////////////////////////////////////////////////////////////////////

int SimpleGrid::CoordinateVectorToIndex(
	const std::vector<int> & coordvec
) const {
	if (m_nGridDim.size() == 0) {
		_EXCEPTIONT("Invalid SimpleGrid");
	}

	if (coordvec.size() != m_nGridDim.size()) {
		_EXCEPTIONT("Invalid coordinate vector");
	}
	if (coordvec.size() == 1) {
		if (coordvec[0] >= m_nGridDim[0]) {
			_EXCEPTIONT("Coordinate vector out of range");
		}
		return coordvec[0];
	}
	if (coordvec.size() == 2) {
		if (coordvec[0] >= m_nGridDim[0]) {
			_EXCEPTIONT("Coordinate vector out of range");
		}
		if (coordvec[1] >= m_nGridDim[1]) {
			_EXCEPTIONT("Coordinate vector out of range");
		}
	}

	int ix = 0;
	int d = 1;
	for (int i = 0; i < coordvec.size(); i++) {
		if (coordvec[i] >= m_nGridDim[i]) {
			_EXCEPTIONT("Coordinate vector out of range");
		}
		ix = ix + i * d;
		d = d * m_nGridDim[i];
	}

	return ix;
}

///////////////////////////////////////////////////////////////////////////////

void SimpleGrid::BuildKDTree() {
	if (m_kdtree != NULL) {
		_EXCEPTIONT("kdtree already exists");
	}
	if (m_dLon.GetRows() == 0) {
		_EXCEPTIONT("At least one grid cell needed in SimpleGrid");
	}

	_ASSERT(m_dLon.GetRows() == m_dLat.GetRows());

	// Create the kd tree
	m_kdtree = kd_create(3);
	if (m_kdtree == NULL) {
		_EXCEPTIONT("kd_create(3) failed");
	}

	// Insert all nodes from this SimpleGrid into the kdtree
	for (size_t i = 0; i < m_dLon.GetRows(); i++) {
		double dX = cos(m_dLon[i]) * cos(m_dLat[i]);
		double dY = sin(m_dLon[i]) * cos(m_dLat[i]);
		double dZ = sin(m_dLat[i]);

		kd_insert3(m_kdtree, dX, dY, dZ, (void*)(i));
	}
}

///////////////////////////////////////////////////////////////////////////////

size_t SimpleGrid::NearestNode(
	double dLonRad,
	double dLatRad
) const {
	if (m_kdtree == NULL) {
		_EXCEPTIONT("BuildKDTree() must be called before NearestNode()");
	}

	// Find the nearest node from a given point
	double dX = cos(dLonRad) * cos(dLatRad);
	double dY = sin(dLonRad) * cos(dLatRad);
	double dZ = sin(dLatRad);

	kdres * kdresNearest = kd_nearest3(m_kdtree, dX, dY, dZ);
	if (kdresNearest == NULL) {
		_EXCEPTIONT("kd_nearest3() failed");
	}
	int nResSize = kd_res_size(kdresNearest);
	if (nResSize != 1) {
		_EXCEPTION1("kd_nearest3() returned incorrect result size (%i)", nResSize);
	}

	size_t i = (size_t)(kd_res_item_data(kdresNearest));

	kd_res_free(kdresNearest);

	return i;
}

///////////////////////////////////////////////////////////////////////////////

