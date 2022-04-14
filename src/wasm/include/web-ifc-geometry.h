/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
 
 #pragma once

#include <string>
#include <vector>
#include <array>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iostream>

#include "../deps/glm/glm/glm.hpp"
#include "../deps/glm/glm/gtx/transform.hpp"
#include "../deps/earcut/include/mapbox/earcut.hpp"

#include "math/intersect-mesh-mesh.h"
#include "math/bool-mesh-mesh.h"


#include "ifc2x4.h"
#include "web-ifc.h"
#include "util.h"

const double EXTRUSION_DISTANCE_HALFSPACE_M = 50;

const bool DEBUG_DUMP_SVG = false;

struct GeometryStatistics
{
	uint32_t meshCacheHits = 0;
	uint32_t meshCacheMisses = 0;
	uint32_t verticesCached = 0;
	uint32_t totalVertices = 0;

	double GetCacheRatio()
	{
		return static_cast<double>(meshCacheHits) / (meshCacheHits + meshCacheMisses);
	}
};

namespace webifc
{
	class IfcGeometryLoader
	{
	public:
		IfcGeometryLoader(IfcLoader& l) :
			_loader(l),
			_transformation(1)
		{

		}

		IfcGeometry& GetCachedGeometry(uint32_t expressID)
		{
			return _expressIDToGeometry[expressID];
		}

		void ClearCachedGeometry()
		{
			_expressIDToGeometry = {};
		}

		bool HasCachedGeometry(uint32_t expressID)
		{
			return _expressIDToGeometry.find(expressID) != _expressIDToGeometry.end();
		}

		IfcGeometry GetFlattenedGeometry(uint32_t expressID)
		{
			auto mesh = GetMesh(expressID);
            return flatten(mesh, _expressIDToGeometry, NormalizeIFC);
		}

		void AddComposedMeshToFlatMesh(IfcFlatMesh& flatMesh, const IfcComposedMesh& composedMesh, const glm::dmat4& parentMatrix = glm::dmat4(1), const glm::dvec4& color = glm::dvec4(1, 1, 1, 1), bool hasColor = false)
		{
			glm::dvec4 newParentColor = color;
			bool newHasColor = hasColor;
			glm::dmat4 newMatrix = parentMatrix * composedMesh.transformation;

			if (composedMesh.hasColor && !hasColor)
			{
				newHasColor = true;
				newParentColor = composedMesh.color;
			}

			if (composedMesh.hasGeometry)
			{
				IfcPlacedGeometry geometry;

				if (!_isCoordinated && _loader.GetSettings().COORDINATE_TO_ORIGIN)
				{
					auto& geom = _expressIDToGeometry[composedMesh.expressID];
					auto pt = geom.GetPoint(0);
					auto transformedPt = newMatrix * glm::dvec4(pt, 1);
					_coordinationMatrix = glm::translate(-glm::dvec3(transformedPt));
					_isCoordinated = true;
				}

				geometry.color = newParentColor;
				geometry.transformation = _coordinationMatrix * newMatrix;
				geometry.SetFlatTransformation();
				geometry.geometryExpressID = composedMesh.expressID;

				flatMesh.geometries.push_back(geometry);
			}

			for (auto& c : composedMesh.children)
			{
				AddComposedMeshToFlatMesh(flatMesh, c, newMatrix, newParentColor, newHasColor);
			}
		}

		IfcFlatMesh GetFlatMesh(uint32_t expressID)
		{
			IfcFlatMesh flatMesh;
			flatMesh.expressID = expressID;

			IfcComposedMesh composedMesh = GetMesh(expressID);

			glm::dmat4 mat = glm::scale(glm::dvec3(_loader.GetLinearScalingFactor()));

			AddComposedMeshToFlatMesh(flatMesh, composedMesh, _transformation * NormalizeIFC * mat);

			return flatMesh;
		}

		IfcComposedMesh GetMesh(uint32_t expressID)
		{
			if (_loader.GetSettings().MESH_CACHE)
			{
				auto it = _expressIDToMesh.find(expressID);

				if (it == _expressIDToMesh.end())
				{
					_statistics.meshCacheMisses++;
					_expressIDToMesh[expressID] = GetMeshByLine(_loader.ExpressIDToLineID(expressID));
				}
				else
				{
					_statistics.meshCacheHits++;
				}

				return _expressIDToMesh[expressID];
			}
			else
			{
				return GetMeshByLine(_loader.ExpressIDToLineID(expressID));
			}
		}

		IfcProfile GetProfile(uint32_t expressID)
		{
			auto profile = GetProfileByLine(_loader.ExpressIDToLineID(expressID));
			if (!profile.curve.IsCCW())
			{
				profile.curve.Invert();
			}
			for (auto& hole : profile.holes)
			{
				if (hole.IsCCW())
				{
					hole.Invert();
				}
			}

			return profile;
		}

		void DumpMesh(IfcComposedMesh& mesh, std::wstring filename)
		{
			size_t offset = 0;
            writeFile(filename, ToObj(mesh, _expressIDToGeometry, offset, NormalizeIFC));
		}

        void SetTransformation(const glm::dmat4& val)
        {
            _transformation = val;
        }

		template<uint32_t DIM>
		IfcCurve<DIM> GetCurve(uint32_t expressID)
		{
			IfcCurve<DIM> curve;
			ComputeCurve<DIM>(expressID, curve);
			return curve;
		}

		GeometryStatistics GetStatistics()
		{
			return _statistics;
		}

		glm::dmat4 GetCoordinationMatrix()
		{
			return _coordinationMatrix;
		}

	private:
        glm::dmat4 _transformation;
		GeometryStatistics _statistics;

		IfcComposedMesh GetMeshByLine(uint32_t lineID)
		{
			auto& line = _loader.GetLine(lineID);

			bool hasColor = false;
			glm::dvec4 styledItemColor(1);
			auto& styledItems = _loader.GetStyledItems();
			auto& relMaterials = _loader.GetRelMaterials();
			auto& materialDefinitions = _loader.GetMaterialDefinitions();
			auto& relVoids = _loader.GetRelVoids();
			auto& relAggregates = _loader.GetRelAggregates();

			auto styledItem = styledItems.find(line.expressID);
			if (styledItem != styledItems.end())
			{
				auto items = styledItem->second;
				for (auto item : items)
				{
					bool success = GetColor(item.second, styledItemColor);
					if (success)
					{
						hasColor = true;
						break;
					}
				}
			}

			if (!hasColor)
			{
				auto material = relMaterials.find(line.expressID);
				if (material != relMaterials.end())
				{
					auto& materials = material->second;
					for (auto item : materials)
					{
						if (materialDefinitions.count(item.second) != 0)
						{
							auto& defs = materialDefinitions[item.second];
							for (auto def : defs)
							{
								bool success = GetColor(def.second, styledItemColor);
								if (success)
								{
									hasColor = true;
									break;
								}
							}
						}

						// if no color found, check material itself
						if (!hasColor)
						{
							bool success = GetColor(item.second, styledItemColor);
							if (success)
							{
								hasColor = true;
							}
						}
					}

				}
			}

			IfcComposedMesh mesh;
			mesh.expressID = line.expressID;
			mesh.hasColor = hasColor;
			mesh.color = styledItemColor;
			mesh.transformation = glm::dmat4(1);

			bool isIfcElement = ifc2x4::IsIfcElement(line.ifcType);
			if (isIfcElement)
			{
				_loader.MoveToArgumentOffset(line, 5);
				uint32_t localPlacement = 0;
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					localPlacement = _loader.GetRefArgument();
				}
				uint32_t ifcPresentation = 0;
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					ifcPresentation = _loader.GetRefArgument();
				}

				if (localPlacement != 0)
				{
					mesh.transformation = GetLocalPlacement(localPlacement);
				}

				if (ifcPresentation != 0)
				{
					mesh.children.push_back(GetMesh(ifcPresentation));
				}

				/*
				// not sure if aggregates are needed here...
				// add aggregates before applying voids!
				auto relAggIt = relAggregates.find(line.expressID);
				if (relAggIt != relAggregates.end() && !relAggIt->second.empty())
				{
					for (auto relAggExpressID : relAggIt->second)
					{
						// hacky fix to avoid double application of the parent matrix
						auto aggMesh = GetMesh(relAggExpressID);
						aggMesh.transformation *= glm::inverse(mesh.transformation);
						mesh.children.push_back(aggMesh);
					}
				}
				*/

				auto relVoidsIt = relVoids.find(line.expressID);

				if (relVoidsIt != relVoids.end() && !relVoidsIt->second.empty())
				{
					IfcComposedMesh resultMesh;

					auto origin = GetOrigin(mesh, _expressIDToGeometry);
					auto normalizeMat = glm::translate(-origin);
					auto flatElementMesh = flatten(mesh, _expressIDToGeometry, normalizeMat);
					auto elementColor = mesh.GetColor();

					if (!flatElementMesh.IsEmpty())
					{

						// TODO: this is inefficient, better make one-to-many subtraction in bool logic
						for (auto relVoidExpressID : relVoidsIt->second)
						{
							IfcComposedMesh voidMesh = GetMesh(relVoidExpressID);
							auto flatVoidMesh = flatten(voidMesh, _expressIDToGeometry, normalizeMat);

							flatElementMesh = BoolSubtract(flatElementMesh, flatVoidMesh);
						}
					}

					_expressIDToGeometry[line.expressID] = flatElementMesh;
					resultMesh.transformation = glm::translate(origin);
					resultMesh.expressID = line.expressID;
					resultMesh.hasGeometry = true;
					if (hasColor)
					{
						resultMesh.color = styledItemColor;
						resultMesh.hasColor = true;
					}
					else if (elementColor.has_value())
					{
						resultMesh.hasColor = true;
						resultMesh.color = *elementColor;
					}
					else
					{
						resultMesh.hasColor = false;
					}

					return resultMesh;
				}
				else
				{
					return mesh;
				}
			}
			else
			{
				switch (line.ifcType)
				{
				case ifc2x4::IFCMAPPEDITEM:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t ifcPresentation = _loader.GetRefArgument();
					uint32_t localPlacement = _loader.GetRefArgument();

					mesh.transformation = GetLocalPlacement(localPlacement);
					mesh.children.push_back(GetMesh(ifcPresentation));
					
					return mesh;
				}
				case ifc2x4::IFCBOOLEANCLIPPINGRESULT:
				{
					_loader.MoveToArgumentOffset(line, 1);
					uint32_t firstOperandID = _loader.GetRefArgument();
					uint32_t secondOperandID = _loader.GetRefArgument();

					auto firstMesh = GetMesh(firstOperandID);
					auto secondMesh = GetMesh(secondOperandID);

					auto origin = GetOrigin(firstMesh, _expressIDToGeometry);
					auto normalizeMat = glm::translate(-origin);

					auto flatFirstMesh = flatten(firstMesh, _expressIDToGeometry, normalizeMat);
					auto flatSecondMesh = flatten(secondMesh, _expressIDToGeometry, normalizeMat);

					webifc::IfcGeometry resultMesh = BoolSubtract(flatFirstMesh, flatSecondMesh);
					
					_expressIDToGeometry[line.expressID] = resultMesh;
					mesh.hasGeometry = true;
					mesh.transformation = glm::translate(origin);

                    if (!mesh.hasColor && firstMesh.hasColor)
                    {
                        mesh.hasColor = true;
                        mesh.color = firstMesh.color;
                    }

					return mesh;
				}
				case ifc2x4::IFCBOOLEANRESULT:
				{
					// @Refactor: duplicate of above

					_loader.MoveToArgumentOffset(line, 0);
					std::string op = _loader.GetStringArgument();

					if (op != "DIFFERENCE")
					{
						_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Unsupported boolean op " + op, line.expressID });
						return mesh;
					}

					uint32_t firstOperandID = _loader.GetRefArgument();
					uint32_t secondOperandID = _loader.GetRefArgument();

					auto firstMesh = GetMesh(firstOperandID);
					auto secondMesh = GetMesh(secondOperandID);

					auto origin = GetOrigin(firstMesh, _expressIDToGeometry);
					auto normalizeMat = glm::translate(-origin);

					auto flatFirstMesh = flatten(firstMesh, _expressIDToGeometry, normalizeMat);
					auto flatSecondMesh = flatten(secondMesh, _expressIDToGeometry, normalizeMat);

					if (flatFirstMesh.numFaces == 0)
					{
						// bail out because we will get strange meshes
						// if this happens, probably there's an issue parsing the first mesh
						return mesh;
					}

					webifc::IfcGeometry resultMesh = BoolSubtract(flatFirstMesh, flatSecondMesh);

					_expressIDToGeometry[line.expressID] = resultMesh;
					mesh.hasGeometry = true;
					mesh.transformation = glm::translate(origin);
                    if (!mesh.hasColor && firstMesh.hasColor)
                    {
                        mesh.hasColor = true;
                        mesh.color = firstMesh.color;
                    }

					return mesh;
				}
				case ifc2x4::IFCHALFSPACESOLID:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t surfaceID = _loader.GetRefArgument();
					std::string agreement = _loader.GetStringArgument();

					IfcSurface surface = GetSurface(surfaceID);

					glm::dvec3 extrusionNormal = glm::dvec3(0, 0, 1);

					bool flipWinding = false;
					if (agreement == "T")
					{
						extrusionNormal *= -1;
						flipWinding = true;
					}

					double d = EXTRUSION_DISTANCE_HALFSPACE_M / _loader.GetLinearScalingFactor();

					IfcProfile profile;
					profile.isConvex = false;
					profile.curve = GetRectangleCurve(d, d, glm::dmat3(1));

					auto geom = Extrude(profile, extrusionNormal, d);

					// @Refactor: duplicate of extrudedareasolid
					if (flipWinding)
					{
						for (uint32_t i = 0; i < geom.numFaces; i++)
						{
							uint32_t temp = geom.indexData[i * 3 + 0];
							temp = geom.indexData[i * 3 + 0];
							geom.indexData[i * 3 + 0] = geom.indexData[i * 3 + 1];
							geom.indexData[i * 3 + 1] = temp;
						}
					}

					mesh.transformation = surface.transformation;
					// TODO: this is getting problematic.....
					_expressIDToGeometry[line.expressID] = geom;
					mesh.hasGeometry = true;

					return mesh;
				}
				case ifc2x4::IFCPOLYGONALBOUNDEDHALFSPACE:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t surfaceID = _loader.GetRefArgument();
					std::string agreement = _loader.GetStringArgument();
					uint32_t positionID = _loader.GetRefArgument();
					uint32_t boundaryID = _loader.GetRefArgument();

					IfcSurface surface = GetSurface(surfaceID);
					glm::dmat4 position = GetLocalPlacement(positionID);
					webifc::IfcCurve<2> curve = GetCurve<2>(boundaryID);

					if (!curve.IsCCW())
					{
						curve.Invert();
					}

					glm::dvec3 extrusionNormal = glm::dvec3(0, 0, 1);
					glm::dvec3 planeNormal = surface.transformation[2];
					glm::dvec3 planePosition = surface.transformation[3];


					glm::dmat4 invPosition = glm::inverse(position);
					glm::dvec3 localPlaneNormal = invPosition * glm::dvec4(planeNormal, 0);
					auto localPlanePos = invPosition * glm::dvec4(planePosition, 1);

					bool flipWinding = false;
					double extrudeDistance = EXTRUSION_DISTANCE_HALFSPACE_M / _loader.GetLinearScalingFactor();

					bool halfSpaceInPlaneDirection = agreement != "T";
					bool extrudeInPlaneDirection = glm::dot(localPlaneNormal, extrusionNormal) > 0;
					bool ignoreDistanceInExtrude = (!halfSpaceInPlaneDirection && extrudeInPlaneDirection) || (halfSpaceInPlaneDirection && !extrudeInPlaneDirection);
					if (ignoreDistanceInExtrude)
					{
						// spec says this should be * 0, but that causes issues for degenerate 0 volume pbhs
						// hopefully we can get away by just inverting it
						extrudeDistance *= -1;
						flipWinding = true;
					}

					IfcProfile profile;
					profile.isConvex = false;
					profile.curve = curve;

					auto geom = Extrude(profile, extrusionNormal, extrudeDistance, localPlaneNormal, localPlanePos);
					//auto geom = Extrude(profile, surface.transformation, extrusionNormal, EXTRUSION_DISTANCE_HALFSPACE);

					// @Refactor: duplicate of extrudedareasolid
					if (flipWinding)
					{
						for (uint32_t i = 0; i < geom.numFaces; i++)
						{
							uint32_t temp = geom.indexData[i * 3 + 0];
							temp = geom.indexData[i * 3 + 0];
							geom.indexData[i * 3 + 0] = geom.indexData[i * 3 + 1];
							geom.indexData[i * 3 + 1] = temp;
						}
					}
					
					if (_loader.GetSettings().DUMP_CSG_MESHES)
					{
						DumpIfcGeometry(geom, L"pbhs.obj");
					}

					// TODO: this is getting problematic.....
					_expressIDToGeometry[line.expressID] = geom;
					mesh.hasGeometry = true;
					mesh.transformation = position;

					return mesh;
				}
				case ifc2x4::IFCREPRESENTATIONMAP:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t axis2Placement = _loader.GetRefArgument();
					uint32_t ifcPresentation = _loader.GetRefArgument();

					mesh.transformation = GetLocalPlacement(axis2Placement);
					mesh.children.push_back(GetMesh(ifcPresentation));

					return mesh;
				}
				case ifc2x4::IFCFACEBASEDSURFACEMODEL:
				case ifc2x4::IFCSHELLBASEDSURFACEMODEL:
				{
					_loader.MoveToArgumentOffset(line, 0);
					auto shells = _loader.GetSetArgument();

					for (auto& shell : shells)
					{
						uint32_t shellRef = _loader.GetRefArgument(shell);
						IfcComposedMesh temp;
						_expressIDToGeometry[shellRef] = GetBrep(shellRef);
						temp.expressID = shellRef;
						temp.hasGeometry = true;
						temp.transformation = glm::dmat4(1);
						mesh.children.push_back(temp);
					}

					return mesh;
				}
				case ifc2x4::IFCFACETEDBREP:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t ifcPresentation = _loader.GetRefArgument();

					_expressIDToGeometry[line.expressID] = GetBrep(ifcPresentation);
					mesh.hasGeometry = true;

					return mesh;
				}
				case ifc2x4::IFCPRODUCTREPRESENTATION:
				case ifc2x4::IFCPRODUCTDEFINITIONSHAPE:
				{
					_loader.MoveToArgumentOffset(line, 2);
					auto representations = _loader.GetSetArgument();

					for (auto& repToken : representations)
					{
						uint32_t repID = _loader.GetRefArgument(repToken);
						mesh.children.push_back(GetMesh(repID));
					}

					return mesh;
				}
				case ifc2x4::IFCSHAPEREPRESENTATION:
				{
					_loader.MoveToArgumentOffset(line, 1);
					auto type = _loader.GetStringArgument();

					if (type != "Body")
					{
						return mesh;
					}

					_loader.MoveToArgumentOffset(line, 3);
					auto repItems = _loader.GetSetArgument();

					for (auto& repToken : repItems)
					{
						uint32_t repID = _loader.GetRefArgument(repToken);
						mesh.children.push_back(GetMesh(repID));
					}

					return mesh;
				}
				case ifc2x4::IFCPOLYGONALFACESET:
				{
					_loader.MoveToArgumentOffset(line, 0);

					auto coordinatesRef = _loader.GetRefArgument();
					auto points = ReadIfcCartesianPointList3D(coordinatesRef);

					// second optional argument closed, ignored

					// indices
					_loader.MoveToArgumentOffset(line, 2);
					auto faces = _loader.GetSetArgument();

					IfcGeometry geom;

					std::vector<IfcBound3D> bounds;
					for (auto& face : faces)
					{
						uint32_t faceID = _loader.GetRefArgument(face);
						ReadIndexedPolygonalFace(faceID, bounds, points);

						TriangulateBounds(geom, bounds);

						bounds.clear();
					}

					_loader.MoveToArgumentOffset(line, 3);
					if (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
					{
						_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Unsupported IFCPOLYGONALFACESET with PnIndex", line.expressID });
					}

					_expressIDToGeometry[line.expressID] = geom;
					mesh.expressID = line.expressID;
					mesh.hasGeometry = true;
					
					return mesh;
				}
				case ifc2x4::IFCTRIANGULATEDFACESET:
				{
					_loader.MoveToArgumentOffset(line, 0);

					auto coordinatesRef = _loader.GetRefArgument();
					auto points = ReadIfcCartesianPointList3D(coordinatesRef);

					// second argument normals, ignored
					// third argument closed, ignored

					// indices
					_loader.MoveToArgumentOffset(line, 3);
					auto indices = Read2DArrayOfThreeIndices();

					IfcGeometry geom;


					_loader.MoveToArgumentOffset(line, 4);
					if (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
					{
						_loader.Reverse();
						auto pnIndex = Read2DArrayOfThreeIndices();

						// ignore
						// std::cout << "Unsupported IFCTRIANGULATEDFACESET with PnIndex!" << std::endl;
					}

					for (int i = 0; i < indices.size(); i += 3)
					{
						int i1 = indices[i + 0] - 1;
						int i2 = indices[i + 1] - 1;
						int i3 = indices[i + 2] - 1;

						geom.AddFace(points[i1], points[i2], points[i3]);
					}

					// DumpIfcGeometry(geom, L"test.obj");

					_expressIDToGeometry[line.expressID] = geom;
					mesh.expressID = line.expressID;
					mesh.hasGeometry = true;
					
					return mesh;
				}
				case ifc2x4::IFCSWEPTDISKSOLID:
				{
					_loader.MoveToArgumentOffset(line, 0);
					auto directrixRef = _loader.GetRefArgument();

					double radius = _loader.GetDoubleArgument();
					double innerRadius = 0.0;

					if (_loader.GetTokenType() == IfcTokenType::REAL)
					{
						_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Inner radius of IFCSWEPTDISKSOLID currently not supported", line.expressID });
						innerRadius = _loader.GetDoubleArgument();
					}

					// TODO: ignoring start/end params for now
					double startParam = 0; // = _loader.GetDoubleArgument();
					double endParam = 0; // = _loader.GetDoubleArgument();

					IfcCurve<3> directrix = GetCurve<3>(directrixRef);

					IfcProfile profile;
					profile.curve = GetCircleCurve(radius, _loader.GetSettings().CIRCLE_SEGMENTS_MEDIUM);

					IfcGeometry geom = Sweep(profile, directrix);

					_expressIDToGeometry[line.expressID] = geom;
					mesh.expressID = line.expressID;
					mesh.hasGeometry = true;

					return mesh;
				}
				case ifc2x4::IFCREVOLVEDAREASOLID:
				{
					IfcComposedMesh mesh;

					_loader.MoveToArgumentOffset(line, 0);
					uint32_t profileID = _loader.GetRefArgument();
					uint32_t placementID = _loader.GetRefArgument();
					uint32_t axis1PlacementID = _loader.GetRefArgument();
					double angle = _loader.GetDoubleArgument();

					IfcProfile profile = GetProfile(profileID);
					glm::dmat4 placement = GetLocalPlacement(placementID);
					glm::dvec3 pos;
					glm::dvec3 axis;

					GetAxis1Placement(axis1PlacementID, pos, axis);

					IfcCurve<3> directrix = BuildArc(pos, axis, angle);

					IfcGeometry geom = Sweep(profile, directrix, axis);

					mesh.transformation = placement;
					_expressIDToGeometry[line.expressID] = geom;
					mesh.expressID = line.expressID;
					mesh.hasGeometry = true;

					mesh.hasColor = hasColor;
					mesh.color = styledItemColor;
					_expressIDToMesh[line.expressID] = mesh;
					return mesh;
				}
				case ifc2x4::IFCEXTRUDEDAREASOLID:
				{
					_loader.MoveToArgumentOffset(line, 0);
					uint32_t profileID = _loader.GetRefArgument();
					uint32_t placementID = _loader.GetOptionalRefArgument();
					uint32_t directionID = _loader.GetRefArgument();
					double depth = _loader.GetDoubleArgument();

					IfcProfile profile = GetProfile(profileID);
					if (profile.curve.points.empty())
					{
						return mesh;
					}

					if (placementID)
					{
						mesh.transformation = GetLocalPlacement(placementID);
					}

					glm::dvec3 dir = GetCartesianPoint3D(directionID);

					double dirDot = glm::dot(dir, glm::dvec3(0, 0, 1));
					bool flipWinding = dirDot < 0; // can't be perp according to spec

					if (DEBUG_DUMP_SVG)
					{
						DumpSVGCurve(profile.curve.points, L"IFCEXTRUDEDAREASOLID_curve.html");
					}

					IfcGeometry geom = Extrude(profile, dir, depth);

					if (flipWinding)
					{
						for (uint32_t i = 0; i < geom.numFaces; i++)
						{
							uint32_t temp = geom.indexData[i * 3 + 0];
							temp = geom.indexData[i * 3 + 0];
							geom.indexData[i * 3 + 0] = geom.indexData[i * 3 + 1];
							geom.indexData[i * 3 + 1] = temp;
						}
					}

					if (DEBUG_DUMP_SVG)
					{
						DumpIfcGeometry(geom, L"IFCEXTRUDEDAREASOLID_geom.obj");
					}

					_expressIDToGeometry[line.expressID] = geom;
					mesh.expressID = line.expressID;
					mesh.hasGeometry = true;
					
					return mesh;
				}
				case ifc2x4::IFCPOLYLINE:
					// ignore polylines as meshes
					return mesh;
				default:
					_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected mesh type", line.expressID, line.ifcType });
					break;
				}
			}

			return IfcComposedMesh();
		}

		IfcCurve<3> BuildArc(const glm::dvec3& pos, const glm::dvec3& axis, double angleRad)
		{
			IfcCurve<3> curve;

			double radius = glm::length(pos);

			// project pos onto axis
			double pdota = glm::dot(axis, pos);
			glm::dvec3 pproja = pdota * axis;

			glm::dvec3 right = -(pos - pproja);
			glm::dvec3 up = glm::cross(axis, right);

			auto curve2D = GetEllipseCurve(1, 1, _loader.GetSettings().CIRCLE_SEGMENTS_MEDIUM, glm::dmat3(1), 0, angleRad, true);

			for (auto& pt2D : curve2D.points)
			{
				glm::dvec3 pt3D = pos + pt2D.x * right + pt2D.y * up;
				curve.Add(pt3D);
			}

			return curve;
		}

		void GetAxis1Placement(uint32_t expressID, glm::dvec3& pos, glm::dvec3& axis)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);
			uint32_t locationID = _loader.GetRefArgument();
			IfcTokenType dirToken = _loader.GetTokenType();


			axis = glm::dvec3(0, 0, 1);
			if (dirToken == IfcTokenType::REF)
			{
				_loader.Reverse();
				axis = GetCartesianPoint3D(_loader.GetRefArgument());
			}

			pos = GetCartesianPoint3D(locationID);
		}

		std::vector<uint32_t> Read2DArrayOfThreeIndices()
		{
			std::vector<uint32_t> result;

			IfcTokenType t = _loader.GetTokenType();

			// while we have point set begin
			while (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
			{
				result.push_back(static_cast<uint32_t>(_loader.GetDoubleArgument()));
				result.push_back(static_cast<uint32_t>(_loader.GetDoubleArgument()));
				result.push_back(static_cast<uint32_t>(_loader.GetDoubleArgument()));

				// read point set end
				_loader.GetTokenType();
			}

			return result;
		}

		IfcGeometry BoolSubtract(const IfcGeometry& firstGeom, const IfcGeometry& secondGeom)
		{
			IfcGeometry result;

			glm::dvec3 center;
			glm::dvec3 extents;
			firstGeom.GetCenterExtents(center, extents);

			auto first = firstGeom.Normalize(center, extents);
			auto second = secondGeom.Normalize(center, extents);

			if (first.numFaces == 0 || second.numFaces == 0)
			{
				_loader.ReportError({ LoaderErrorType::BOOL_ERROR, "bool aborted due to empty source or target" });

				// bail out because we will get strange meshes
				// if this happens, probably there's an issue parsing the mesh that occurred earlier
				return firstGeom;
			}

			if (_loader.GetSettings().DUMP_CSG_MESHES)
			{
				DumpIfcGeometry(first, L"first.obj");
				DumpIfcGeometry(second, L"second.obj");
			}

			if (_loader.GetSettings().USE_FAST_BOOLS)
			{
				IfcGeometry r1;
				IfcGeometry r2;

				intersectMeshMesh(first, second, r1, r2);

				if (_loader.GetSettings().DUMP_CSG_MESHES)
				{
					DumpIfcGeometry(r1, L"r1.obj");
					DumpIfcGeometry(r2, L"r2.obj");
				}

				result = boolSubtract(r1, r2);
			}
			else
			{
				const int threshold = LoaderSettings().BOOL_ABORT_THRESHOLD;

				if (firstGeom.numPoints > threshold || secondGeom.numPoints > threshold)
				{
					_loader.ReportError({ LoaderErrorType::BOOL_ERROR, "complex bool aborted due to BOOL_ABORT_THRESHOLD" });

					// bail out because we expect this operation to take too long
					return firstGeom;
				}

				result = boolSubtract_CSGJSCPP(first, second);
			}

			if (_loader.GetSettings().DUMP_CSG_MESHES)
			{
				DumpIfcGeometry(result, L"result.obj");
			}

			return result.DeNormalize(center, extents);
		}

		std::vector<glm::dvec3> ReadIfcCartesianPointList3D(uint32_t expressID)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);

			std::vector<glm::dvec3> result;

			IfcTokenType t = _loader.GetTokenType();

			// while we have point set begin
			while (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
			{
				// because these calls cannot be reordered we have to use intermediate variables
				double x = _loader.GetDoubleArgument();
				double y = _loader.GetDoubleArgument();
				double z = _loader.GetDoubleArgument();
				result.emplace_back(x, y, z);

				// read point set end
				_loader.GetTokenType();
			}

			return result;
		}

		std::vector<glm::dvec2> ReadIfcCartesianPointList2D(uint32_t expressID)
		{
			// TODO: near-duplicate of 3D, can make template
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);

			std::vector<glm::dvec2> result;

			IfcTokenType t = _loader.GetTokenType();

			// while we have point set begin
			while (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
			{
				// because these calls cannot be reordered we have to use intermediate variables
				double x = _loader.GetDoubleArgument();
				double y = _loader.GetDoubleArgument();
				result.emplace_back(x, y);

				// read point set end
				_loader.GetTokenType();
			}

			return result;
		}

		void ReadIndexedPolygonalFace(uint32_t expressID, std::vector<IfcBound3D>& bounds, const std::vector<glm::dvec3>& points)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			bounds.emplace_back();

			switch (line.ifcType)
			{
			case ifc2x4::IFCINDEXEDPOLYGONALFACEWITHVOIDS:
			case ifc2x4::IFCINDEXEDPOLYGONALFACE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto indexIDs = _loader.GetSetArgument();

				IfcGeometry geometry;
				for (auto& indexID : indexIDs)
				{
					uint32_t index = static_cast<uint32_t>(_loader.GetDoubleArgument(indexID));
					glm::dvec3 point = points[index - 1]; // indices are 1-based
					
					// I am not proud of this
					bounds.back().curve.points.push_back(point);
				}

				if (line.ifcType == ifc2x4::IFCINDEXEDPOLYGONALFACE)
				{
					break;
				}

				// case IFCINDEXEDPOLYGONALFACEWITHVOIDS
				_loader.MoveToArgumentOffset(line, 1);

				// guaranteed to be set begin
				IfcTokenType t = _loader.GetTokenType();

				// while we have hole-index set begin
				while (_loader.GetTokenType() == IfcTokenType::SET_BEGIN)
				{
					bounds.emplace_back();

					while (_loader.GetTokenType() != IfcTokenType::SET_END)
					{
						_loader.Reverse();
						uint32_t index = static_cast<uint32_t>(_loader.GetDoubleArgument());

						glm::dvec3 point = points[index - 1]; // indices are still 1-based

						// I am also not proud of this
						bounds.back().curve.points.push_back(point);
					}
				}

				break;
			}
			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected indexedface type", line.expressID, line.ifcType });
				break;
			}
		}

		IfcGeometry GetBrep(uint32_t expressID)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCCONNECTEDFACESET:
			case ifc2x4::IFCCLOSEDSHELL:
			case ifc2x4::IFCOPENSHELL:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto faces = _loader.GetSetArgument();

				IfcGeometry geometry;
				for (auto& faceToken : faces)
				{
					uint32_t faceID = _loader.GetRefArgument(faceToken);
					AddFaceToGeometry(faceID, geometry);
				}

				return geometry;
			}
			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected shell type", line.expressID, line.ifcType });
				break;
			}

			return IfcGeometry();
		}

		bool GetColor(uint32_t expressID, glm::dvec4& outputColor)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCPRESENTATIONSTYLEASSIGNMENT:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto ifcPresentationStyleSelects = _loader.GetSetArgument();

				for (auto& styleSelect : ifcPresentationStyleSelects)
				{
					uint32_t styleSelectID = _loader.GetRefArgument(styleSelect);
					glm::dvec4 color;
					bool foundColor = GetColor(styleSelectID, color);
					if (foundColor)
					{
						outputColor = color;
						return true;
					}
				}

				return false;
			}
			case ifc2x4::IFCSURFACESTYLE:
			{
				_loader.MoveToArgumentOffset(line, 2);
				auto ifcSurfaceStyleElementSelects = _loader.GetSetArgument();

				for (auto& styleElementSelect : ifcSurfaceStyleElementSelects)
				{
					uint32_t styleElementSelectID = _loader.GetRefArgument(styleElementSelect);
					glm::dvec4 color;
					bool foundColor = GetColor(styleElementSelectID, color);
					if (foundColor)
					{
						outputColor = color;
						return true;
					}
				}

				return false;
			}
			case ifc2x4::IFCSURFACESTYLERENDERING:
			{
				_loader.MoveToArgumentOffset(line, 0);
				GetColor(_loader.GetRefArgument(), outputColor);
				_loader.MoveToArgumentOffset(line, 1);

				if (_loader.GetTokenType() == IfcTokenType::REAL)
				{
					_loader.Reverse();
					outputColor.a = 1 - _loader.GetDoubleArgument();
				}

				return true;
			}
			case ifc2x4::IFCSURFACESTYLESHADING:
			{
				_loader.MoveToArgumentOffset(line, 0);
				GetColor(_loader.GetRefArgument(), outputColor);

				return true;
			}
			case ifc2x4::IFCSTYLEDREPRESENTATION:
			{
				_loader.MoveToArgumentOffset(line, 3);
				auto repItems = _loader.GetSetArgument();

				for (auto& repItem : repItems)
				{
					uint32_t repItemID = _loader.GetRefArgument(repItem);
					glm::dvec4 color;
					bool foundColor = GetColor(repItemID, color);
					if (foundColor)
					{
						outputColor = color;
						return true;
					}
				}

				return false;
			}
			case ifc2x4::IFCSTYLEDITEM:
			{
				_loader.MoveToArgumentOffset(line, 1);
				auto styledItems = _loader.GetSetArgument();

				for (auto& styledItem : styledItems)
				{
					uint32_t styledItemID = _loader.GetRefArgument(styledItem);
					glm::dvec4 color;
					bool foundColor = GetColor(styledItemID, color);
					if (foundColor)
					{
						outputColor = color;
						return true;
					}
				}

				return false;
			}
			case ifc2x4::IFCCOLOURRGB:
			{
				_loader.MoveToArgumentOffset(line, 1);
				outputColor.r = _loader.GetDoubleArgument();
				outputColor.g = _loader.GetDoubleArgument();
				outputColor.b = _loader.GetDoubleArgument();
				outputColor.a = 1;

				return true;
			}
			case ifc2x4::IFCMATERIALLAYERSETUSAGE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				uint32_t layerSetID = _loader.GetRefArgument();
				return GetColor(layerSetID, outputColor);
			}
			case ifc2x4::IFCMATERIALLAYERSET:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto layers = _loader.GetSetArgument();

				for (auto& layer : layers)
				{
					uint32_t layerID = _loader.GetRefArgument(layer);
					glm::dvec4 color;
					bool foundColor = GetColor(layerID, color);
					if (foundColor)
					{
						outputColor = color;
						return true;
					}
				}

				return false;
			}
			case ifc2x4::IFCMATERIALLAYER:
			{
				_loader.MoveToArgumentOffset(line, 0);
				uint32_t matRepID = _loader.GetRefArgument();
				return GetColor(matRepID, outputColor);
			}
			case ifc2x4::IFCMATERIAL:
			{
				if (_loader.GetMaterialDefinitions().count(line.expressID) != 0)
				{
					auto& defs = _loader.GetMaterialDefinitions()[line.expressID];
					for (auto def : defs)
					{
						bool success = GetColor(def.second, outputColor);
						if (success)
						{
							return true;
						}
					}

					return false;
				}

				return false;
			}
			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected style type", line.expressID, line.ifcType });
				break;
			}

			return false;
		}

		void AddFaceToGeometry(uint32_t expressID, IfcGeometry& geometry)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			switch (line.ifcType)
			{
			case ifc2x4::IFCFACE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto bounds = _loader.GetSetArgument();

				std::vector<IfcBound3D> bounds3D(bounds.size());

				for (int i = 0; i < bounds.size(); i++)
				{
					uint32_t boundID = _loader.GetRefArgument(bounds[i]);
					bounds3D[i] = GetBound(boundID);
				}

				TriangulateBounds(geometry, bounds3D);
				break;
			}

			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected face type", line.expressID, line.ifcType });
				break;
			}
		}

		IfcBound3D GetBound(uint32_t expressID)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			switch (line.ifcType)
			{
			case ifc2x4::IFCFACEOUTERBOUND:
			{
				_loader.MoveToArgumentOffset(line, 0);
				uint32_t loop = _loader.GetRefArgument();
				// IfcToken orientation = tokens[_loader.GetArgumentOffset(tokens, 1)];

				IfcBound3D bound;
				bound.curve = GetLoop(loop);
				bound.orientation = true;
				bound.type = IfcBoundType::OUTERBOUND;

				return bound;
			}
			case ifc2x4::IFCFACEBOUND:
			{
				_loader.MoveToArgumentOffset(line, 0);
				uint32_t loop = _loader.GetRefArgument();
				// IfcToken orientation = tokens[_loader.GetArgumentOffset(tokens, 1)];

				IfcBound3D bound;
				bound.curve = GetLoop(loop);
				bound.orientation = true;
				bound.type = IfcBoundType::BOUND;

				return bound;
			}

			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected bound type", line.expressID, line.ifcType });
				break;
			}

			return IfcBound3D();
		}

		IfcCurve3D GetLoop(uint32_t expressID)
		{
			auto lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			switch (line.ifcType)
			{
			case ifc2x4::IFCPOLYLOOP:
			{
				IfcCurve3D curve;

				_loader.MoveToArgumentOffset(line, 0);
				auto points = _loader.GetSetArgument();

				curve.points.reserve(points.size());

				uint32_t prevID = 0;
				for (auto& token : points)
				{
					uint32_t pointId = _loader.GetRefArgument(token);
					
					// trim out consecutive equal points
					if (pointId != prevID)
					{
						curve.points.push_back(GetCartesianPoint3D(pointId));
					}

					prevID = pointId;
				}

				return curve;
			}

			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected loop type", line.expressID, line.ifcType });
				break;
			}

			IfcCurve3D curve;
			return curve;
		}

		void TriangulateBounds(IfcGeometry& geometry, std::vector<IfcBound3D>& bounds)
		{
			if (bounds.size() == 1 && bounds[0].curve.points.size() == 3)
			{
				auto c = bounds[0].curve;

				size_t offset = geometry.numPoints;

				geometry.AddFace(c.points[0], c.points[1], c.points[2]);
			}
			else if (bounds.size() > 0 && bounds[0].curve.points.size() >= 3)
			{
				// bound greater than 4 vertices or with holes, triangulate
				// TODO: modify to use glm::dvec2 with custom accessors
				using Point = std::array<double, 2>;
				std::vector<std::vector<Point>> polygon;

				uint32_t offset = geometry.numPoints;
				
                // if more than one bound
				if (bounds.size() > 1) {
                    // locate the outer bound index
                    int outerIndex = -1;
                    for (int i = 0; i < bounds.size(); i++) {
                        if (bounds[i].type == IfcBoundType::OUTERBOUND) {
                            outerIndex = i;
                            break;
                        }
                    }

					if (outerIndex == -1)
					{
						_loader.ReportError({ LoaderErrorType::PARSING, "Expected outer bound!" });
					}
					else
					{
						// swap the outer bound to the first position
						std::swap(bounds[0], bounds[outerIndex]);
					}
                }

                // if the first bound is not an outer bound now, this is unexpected
                if(bounds[0].type != IfcBoundType::OUTERBOUND)
                {
				    _loader.ReportError({ LoaderErrorType::PARSING, "Expected outer bound first!" });
                }

				glm::dvec3 v1, v2, v3;
				if (!GetBasisFromCoplanarPoints(bounds[0].curve.points, v1, v2, v3))
				{
					// these points are on a line
					_loader.ReportError({ LoaderErrorType::PARSING, "No basis found for brep!" });
					return;
				}

				glm::dvec3 v12(glm::normalize(v3 - v2));
				glm::dvec3 v13(glm::normalize(v1 - v2));
				glm::dvec3 n = glm::normalize(glm::cross(v12, v13));
				v12 = glm::cross(v13, n);

				// check winding of outer bound
				IfcCurve<2> test;
				for (int i = 0; i < bounds[0].curve.points.size(); i++)
				{
					glm::dvec3 pt = bounds[0].curve.points[i];
					glm::dvec3 pt2 = pt - v1;

					glm::dvec2 proj(
						glm::dot(pt2, v12),
						glm::dot(pt2, v13)
					);

					test.Add(proj);
				}

				// if the outer bound is clockwise under the current projection (v12,v13,n), we invert the projection
				if (!test.IsCCW())
				{
					n *= -1;
					std::swap(v12, v13);
				}

				for (auto& bound : bounds)
				{
					std::vector<Point> points;
					for (int i = 0; i < bound.curve.points.size(); i++)
					{
						glm::dvec3 pt = bound.curve.points[i];
						geometry.AddPoint(pt, n);

						// project pt onto plane of curve to obtain 2d coords
						glm::dvec3 pt2 = pt - v1;

						glm::dvec2 proj(
							glm::dot(pt2, v12),
							glm::dot(pt2, v13)
						);

						points.push_back({ proj.x, proj.y });
					}

					polygon.push_back(points);
				}


				std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(polygon);

				for (int i = 0; i < indices.size(); i += 3)
				{
					geometry.AddFace(offset + indices[i + 0], offset + indices[i + 1], offset + indices[i + 2]);
				}
			}
			else
			{
				_loader.ReportError({ LoaderErrorType::PARSING, "bad bound" });
			}
		}

		//! This implementation generates much more vertices than needed, and does not have smoothed normals
		IfcGeometry Sweep(const IfcProfile& profile, const IfcCurve<3>& directrix, const glm::dvec3& initialDirectrixNormal = glm::dvec3(0))
		{
			IfcGeometry geom;

			auto& dpts = directrix.points;

			if (dpts.size() <= 1)
			{
				// nothing to sweep
				return geom;
			}

			// compute curve for each part of the directrix
			std::vector<IfcCurve<3>> curves;

			for (int i = 0; i < dpts.size(); i++)
			{
				IfcCurve<3> segmentForCurve;

				glm::dvec3 planeNormal;
				glm::dvec3 directrixSegmentNormal;
				glm::dvec3 planeOrigin;

				if (i == 0) // start
				{
					planeNormal = glm::normalize(dpts[1] - dpts[0]);
					directrixSegmentNormal = planeNormal;
					planeOrigin = dpts[0];
				}
				else if (i == dpts.size() - 1) // end
				{
					planeNormal = glm::normalize(dpts[i] - dpts[i - 1]);
					directrixSegmentNormal = planeNormal;
					planeOrigin = dpts[i];
				}
				else // middle
				{
					// possibly the directrix is bad
					glm::dvec3 n1 = glm::normalize(dpts[i] - dpts[i - 1]);
					glm::dvec3 n2 = glm::normalize(dpts[i + 1] - dpts[i]);
					glm::dvec3 p = glm::normalize(glm::cross(n1, n2));

					if (std::isnan(p.x))
					{
						// TODO: sometimes outliers cause the perp to become NaN! 
						// this is bad news, as it nans the points added to the final mesh
						// also, it's hard to bail out now :/
						// see curve.add() for more info on how this is currently "solved"
						printf("NaN perp!\n");
					}

					glm::dvec3 u1 = glm::normalize(glm::cross(n1, p));
					glm::dvec3 u2 = glm::normalize(glm::cross(n2, p));
					glm::dvec3 au = glm::normalize(u1 + u2);
					planeNormal = glm::normalize(glm::cross(au, p));
					directrixSegmentNormal = n1; // n1 or n2 doesn't matter

					planeOrigin = dpts[i];
				}

				if (curves.empty())
				{
					// construct initial curve
					glm::dvec3 left;
					if (initialDirectrixNormal == glm::dvec3(0))
					{
						left = glm::cross(directrixSegmentNormal, glm::dvec3(directrixSegmentNormal.y, directrixSegmentNormal.x, directrixSegmentNormal.z));
						if (left == glm::dvec3(0, 0, 0))
						{
							left = glm::cross(directrixSegmentNormal, glm::dvec3(directrixSegmentNormal.x, directrixSegmentNormal.z, directrixSegmentNormal.y));
						}
					}
					else
					{
						left = glm::cross(directrixSegmentNormal, initialDirectrixNormal);
					}

					if (left == glm::dvec3(0, 0, 0))
					{
						printf("0 left vec in sweep!\n");
					}

					glm::dvec3 right = glm::normalize(glm::cross(directrixSegmentNormal, left));
					left = glm::normalize(glm::cross(directrixSegmentNormal, right));

					// project profile onto planeNormal, place on planeOrigin
					// TODO: look at holes
					auto& ppts = profile.curve.points;
					for (auto& pt2D : ppts)
					{
						glm::dvec3 pt = pt2D.x * left + pt2D.y * right + planeOrigin;
						glm::dvec3 proj = projectOntoPlane(planeOrigin, planeNormal, pt, directrixSegmentNormal);

						segmentForCurve.Add(pt);
					}
				}
				else
				{
					// project previous curve onto the normal
					const IfcCurve<3>& prevCurve = curves.back();

					auto& ppts = prevCurve.points;
					for (auto& pt : ppts)
					{
						glm::dvec3 proj = projectOntoPlane(planeOrigin, planeNormal, pt, directrixSegmentNormal);

						segmentForCurve.Add(proj);
					}
				}
				
				curves.push_back(segmentForCurve);
			}

			// connect the curves
			for (int i = 1; i < dpts.size(); i++)
			{
				const auto& c1 = curves[i - 1].points;
				const auto& c2 = curves[i].points;

				uint32_t capSize = c1.size();
				for (int j = 1; j < capSize; j++)
				{
					glm::dvec3 bl = c1[j - 1];
					glm::dvec3 br = c1[j - 0];

					glm::dvec3 tl = c2[j - 1];
					glm::dvec3 tr = c2[j - 0];

					geom.AddFace(tl, br, bl);
					geom.AddFace(tl, tr, br);
				}
			}

			//DumpSVGCurve(directrix.points, glm::dvec3(), L"directrix.html");
			//DumpIfcGeometry(geom, L"sweep.obj");

			return geom;
		}

		IfcGeometry Extrude(IfcProfile profile, glm::dvec3 dir, double distance, glm::dvec3 cuttingPlaneNormal = glm::dvec3(0), glm::dvec3 cuttingPlanePos = glm::dvec3(0))
		{
			IfcGeometry geom;
			std::vector<bool> holesIndicesHash;

			// build the caps
			{
				using Point = std::array<double, 2>;
				int polygonCount = 1 + profile.holes.size(); //Main profile + holes
				std::vector<std::vector<Point>> polygon(polygonCount);

				glm::dvec3 normal = dir;

				for (int i = 0; i < profile.curve.points.size(); i++)
				{
					glm::dvec2 pt = profile.curve.points[i];
					glm::dvec4 et = glm::dvec4(glm::dvec3(pt, 0) + dir * distance, 1);

					geom.AddPoint(et, normal);
					polygon[0].push_back({ pt.x, pt.y });
				}

				for(int i = 0; i < profile.curve.points.size(); i++)
				{
					holesIndicesHash.push_back(false);
				}

				for (int i = 0; i < profile.holes.size(); i++)
				{
					IfcCurve<2> hole = profile.holes[i];
					int pointCount = hole.points.size();

					for (int j = 0; j < pointCount; j++)
					{
						holesIndicesHash.push_back(j == 0);

						glm::dvec2 pt = hole.points[j];
						glm::dvec4 et = glm::dvec4(glm::dvec3(pt, 0) + dir * distance, 1);

						profile.curve.Add(pt);
						geom.AddPoint(et, normal);
						polygon[i + 1].push_back({ pt.x, pt.y }); //Index 0 is main profile; see earcut reference
					}
				}

				std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(polygon);

				if (indices.size() < 3)
				{
					// probably a degenerate polygon
					_loader.ReportError({ LoaderErrorType::UNSPECIFIED, "degenerate polygon in extrude" });
					return geom;
				}

				uint32_t offset = 0;
				bool winding = GetWindingOfTriangle(geom.GetPoint(offset + indices[0]), geom.GetPoint(offset + indices[1]), geom.GetPoint(offset + indices[2]));
				bool flipWinding = !winding;

				for (int i = 0; i < indices.size(); i += 3)
				{
					if (flipWinding)
					{
						geom.AddFace(offset + indices[i + 0], offset + indices[i + 2], offset + indices[i + 1]);
					}
					else
					{
						geom.AddFace(offset + indices[i + 0], offset + indices[i + 1], offset + indices[i + 2]);
					}
				}

				offset += geom.numPoints;

				normal = -dir;

				for (int i = 0; i < profile.curve.points.size(); i++)
				{
					glm::dvec2 pt = profile.curve.points[i];
					glm::dvec4 et = glm::dvec4(glm::dvec3(pt, 0), 1);

					if (cuttingPlaneNormal != glm::dvec3(0))
					{
						et = glm::dvec4(glm::dvec3(pt, 0), 1);
						glm::dvec3 transDir = glm::dvec4(dir, 0);

						// project {et} onto the plane, following the extrusion normal						
						double ldotn = glm::dot(transDir, cuttingPlaneNormal);
						if (ldotn == 0)
						{
							printf("0 direction in extrude\n");
						}
						else
						{
							glm::dvec3 dpos = cuttingPlanePos - glm::dvec3(et);
							double dist = glm::dot(dpos, cuttingPlaneNormal) / ldotn;
							// we want to apply dist, even when negative
							et = et + glm::dvec4(dist * transDir, 1);
						}
					}

					geom.AddPoint(et, normal);
				}

				for (int i = 0; i < indices.size(); i += 3)
				{
					if (flipWinding)
					{
						geom.AddFace(offset + indices[i + 0], offset + indices[i + 1], offset + indices[i + 2]);
					}
					else
					{
						geom.AddFace(offset + indices[i + 0], offset + indices[i + 2], offset + indices[i + 1]);
					}
				}
			}

			uint32_t capSize = profile.curve.points.size();
			for (int i = 1; i < capSize; i++)
			{
				//https://github.com/tomvandig/web-ifc/issues/5
				if (holesIndicesHash[i])
				{
					continue;
				}

				uint32_t bl = i - 1;
				uint32_t br = i - 0;

				uint32_t tl = capSize + i - 1;
				uint32_t tr = capSize + i - 0;

				// this winding should be correct
				geom.AddFace(geom.GetPoint(tl),
					geom.GetPoint(br),
					geom.GetPoint(bl));

				geom.AddFace(geom.GetPoint(tl),
					geom.GetPoint(tr),
					geom.GetPoint(br));
			}

			return geom;
		}

		bool IsCurveConvex(IfcCurve<2>& curve)
		{
			for (int i = 2; i < curve.points.size(); i++)
			{
				glm::dvec2 a = curve.points[i - 2];
				glm::dvec2 b = curve.points[i - 1];
				glm::dvec2 c = curve.points[i - 0];

				if (!isConvexOrColinear(a, b, c))
				{
					return false;
				}
			}

			return true;
		}


		IfcProfile GetProfileByLine(uint32_t lineID)
		{
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCARBITRARYCLOSEDPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				_loader.MoveToArgumentOffset(line, 2);
				profile.curve = GetCurve<2>(_loader.GetRefArgument());
				profile.isConvex = IsCurveConvex(profile.curve);

				return profile;
			}
			case ifc2x4::IFCARBITRARYPROFILEDEFWITHVOIDS:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				_loader.MoveToArgumentOffset(line, 2);
				profile.curve = GetCurve<2>(_loader.GetRefArgument());
				profile.isConvex = IsCurveConvex(profile.curve);

				_loader.MoveToArgumentOffset(line, 3);
				auto holes = _loader.GetSetArgument();
				
				for (auto& hole : holes)
				{
					IfcCurve<2> holeCurve = GetCurve<2>(_loader.GetRefArgument(hole));
					profile.holes.push_back(holeCurve);
				}

				return profile;
			}
			case ifc2x4::IFCRECTANGLEPROFILEDEF:
			case ifc2x4::IFCROUNDEDRECTANGLEPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t placementID = _loader.GetRefArgument();
				double xdim = _loader.GetDoubleArgument();
				double ydim = _loader.GetDoubleArgument();

				glm::dmat3 placement = GetAxis2Placement2D(placementID);

				profile.curve = GetRectangleCurve(xdim, ydim, placement);

				return profile;
			}
			case ifc2x4::IFCRECTANGLEHOLLOWPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t placementID = _loader.GetRefArgument();
				double xdim = _loader.GetDoubleArgument();
				double ydim = _loader.GetDoubleArgument();
				double thickness = _loader.GetDoubleArgument();

				// fillets not implemented yet

				glm::dmat3 placement = GetAxis2Placement2D(placementID);

				profile.curve = GetRectangleCurve(xdim, ydim, placement);
				profile.holes.push_back(GetRectangleCurve(xdim - thickness, ydim - thickness, placement));
				std::reverse(profile.holes[0].points.begin(), profile.holes[0].points.end());

				return profile;
			}
			case ifc2x4::IFCCIRCLEPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t placementID = _loader.GetOptionalRefArgument();
				double radius = _loader.GetDoubleArgument();
				
				glm::dmat3 placement(1);
				
				if (placementID)
				{
					GetAxis2Placement2D(placementID);
				}

				profile.curve = GetCircleCurve(radius, _loader.GetSettings().CIRCLE_SEGMENTS_HIGH, placement);

				return profile;
			}
			case ifc2x4::IFCELLIPSEPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t placementID = _loader.GetRefArgument();
				double radiusX = _loader.GetDoubleArgument();
				double radiusY = _loader.GetDoubleArgument();

				glm::dmat3 placement = GetAxis2Placement2D(placementID);

				profile.curve = GetEllipseCurve(radiusX, radiusY, _loader.GetSettings().CIRCLE_SEGMENTS_HIGH, placement);

				return profile;
			}
			case ifc2x4::IFCCIRCLEHOLLOWPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t placementID = _loader.GetRefArgument();
				double radius = _loader.GetDoubleArgument();
				double thickness = _loader.GetDoubleArgument();

				glm::dmat3 placement = GetAxis2Placement2D(placementID);

				profile.curve = GetCircleCurve(radius, _loader.GetSettings().CIRCLE_SEGMENTS_HIGH, placement);
				profile.holes.push_back(GetCircleCurve(radius - thickness, _loader.GetSettings().CIRCLE_SEGMENTS_HIGH, placement));
				std::reverse(profile.holes[0].points.begin(), profile.holes[0].points.end());

				return profile;
			}
			case ifc2x4::IFCISHAPEPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = true;

				_loader.MoveToArgumentOffset(line, 2);

				glm::dmat3 placement(1);

				if (_loader.GetTokenType() == webifc::IfcTokenType::REF)
				{
					_loader.Reverse();
				
					uint32_t placementID = _loader.GetRefArgument();
					glm::dmat3 placement = GetAxis2Placement2D(placementID);
				}

				_loader.MoveToArgumentOffset(line, 3);

				double width = _loader.GetDoubleArgument();
				double depth = _loader.GetDoubleArgument();
				double webThickness = _loader.GetDoubleArgument();
				double flangeThickness = _loader.GetDoubleArgument();

				// optional fillet
				bool hasFillet = false;
				double filletRadius = 0;
				if (_loader.GetTokenType() == webifc::IfcTokenType::REAL)
				{
					_loader.Reverse();

					hasFillet = true;
					filletRadius = _loader.GetDoubleArgument();
				}

				profile.curve = GetIShapedCurve(width, depth, webThickness, flangeThickness, hasFillet, filletRadius, placement);

				return profile;
			}
			case ifc2x4::IFCLSHAPEPROFILEDEF:
			{
				IfcProfile profile;

				_loader.MoveToArgumentOffset(line, 0);
				profile.type = _loader.GetStringArgument();
				profile.isConvex = false;

				_loader.MoveToArgumentOffset(line, 2);

				glm::dmat3 placement(1);

				if (_loader.GetTokenType() == webifc::IfcTokenType::REF)
				{
					_loader.Reverse();
				
					uint32_t placementID = _loader.GetRefArgument();
					glm::dmat3 placement = GetAxis2Placement2D(placementID);
				}

				_loader.MoveToArgumentOffset(line, 3);
				double filletRadius = 0;
				double depth = _loader.GetDoubleArgument();
				double width = _loader.GetDoubleArgument();
				double thickness = _loader.GetDoubleArgument();
				filletRadius = _loader.GetDoubleArgument();
				double edgeRadius = _loader.GetDoubleArgument();
				double legSlope = _loader.GetDoubleArgument();
				double centreOfGravityInX = _loader.GetDoubleArgument();
				double centreOfGravityInY = _loader.GetDoubleArgument();

				// optional fillet
				bool hasFillet = false;

				if (_loader.GetTokenType() == webifc::IfcTokenType::REAL)
				{
					_loader.Reverse();

					hasFillet = true;
					filletRadius = _loader.GetDoubleArgument();
				}

				profile.curve = GetLShapedCurve(width, depth, thickness, hasFillet, filletRadius, edgeRadius, legSlope, placement);

				return profile;
			}
			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected profile type", line.expressID, line.ifcType });
				break;
			}

			return IfcProfile();
		}

		IfcSurface GetSurface(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCPLANE:
			{
				IfcSurface surface;

				_loader.MoveToArgumentOffset(line, 0);
				uint32_t locationID = _loader.GetRefArgument();
				surface.transformation = GetLocalPlacement(locationID);

				return surface;
			}
			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected surface type", line.expressID, line.ifcType });
				break;
			}

			return IfcSurface();
		}

		glm::dmat3 GetAxis2Placement2D(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);
			uint32_t locationID = _loader.GetRefArgument();
			IfcTokenType dirToken = _loader.GetTokenType();

			glm::dvec2 xAxis = glm::dvec2(1, 0);
			if (dirToken == IfcTokenType::REF)
			{
				_loader.Reverse();
				xAxis = GetCartesianPoint2D(_loader.GetRefArgument());
			}

			glm::dvec2 pos = GetCartesianPoint2D(locationID);

			glm::dvec2 yAxis = glm::dvec2(xAxis.y, -xAxis.x);

			return glm::dmat3(
				glm::dvec3(xAxis, 0),
				glm::dvec3(yAxis, 0),
				glm::dvec3(pos, 1)
			);
		}

		glm::dmat4 GetLocalPlacement(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCAXIS2PLACEMENT3D:
			{
				glm::dvec3 zAxis(0, 0, 1);
				glm::dvec3 xAxis(1, 0, 0);

				_loader.MoveToArgumentOffset(line, 0);
				uint32_t posID = _loader.GetRefArgument();
				IfcTokenType zID = _loader.GetTokenType();
				if (zID == IfcTokenType::REF)
				{
					_loader.Reverse();
					zAxis = GetCartesianPoint3D(_loader.GetRefArgument());
				}

				_loader.MoveToArgumentOffset(line, 2);
				IfcTokenType xID = _loader.GetTokenType();
				if (xID == IfcTokenType::REF)
				{
					_loader.Reverse();
					xAxis = GetCartesianPoint3D(_loader.GetRefArgument());
				}

				glm::dvec3 pos = GetCartesianPoint3D(posID);


					
				glm::dvec3 yAxis = glm::cross(zAxis, xAxis);

				return glm::dmat4(
					glm::dvec4(xAxis, 0),
					glm::dvec4(yAxis, 0),
					glm::dvec4(zAxis, 0),
					glm::dvec4(pos, 1)
				);
			}
			case ifc2x4::IFCLOCALPLACEMENT:
			{
				glm::dmat4 relPlacement(1);
				
				_loader.MoveToArgumentOffset(line, 0);
				IfcTokenType relPlacementToken = _loader.GetTokenType();
				if (relPlacementToken == IfcTokenType::REF)
				{
					_loader.Reverse();
					relPlacement = GetLocalPlacement(_loader.GetRefArgument());
				}

				_loader.MoveToArgumentOffset(line, 1);
				uint32_t axis2PlacementID = _loader.GetRefArgument();

				glm::dmat4 axis2Placement = GetLocalPlacement(axis2PlacementID);


				auto result = relPlacement * axis2Placement;
				return result;;
			}
			case ifc2x4::IFCCARTESIANTRANSFORMATIONOPERATOR3D:
			case ifc2x4::IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM:
			{
				double scale1 = 1.0;
				double scale2 = 1.0;
				double scale3 = 1.0;

				glm::dvec3 Axis1(1, 0, 0);
				glm::dvec3 Axis2(0, 1, 0);
				glm::dvec3 Axis3(0, 0, 1);


				_loader.MoveToArgumentOffset(line, 0);
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					Axis1 = GetCartesianPoint3D(_loader.GetRefArgument());
				}
				_loader.MoveToArgumentOffset(line, 1);
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					Axis2 = GetCartesianPoint3D(_loader.GetRefArgument());
				}

				_loader.MoveToArgumentOffset(line, 2);
				uint32_t posID = _loader.GetRefArgument();
				glm::dvec3 pos = GetCartesianPoint3D(posID);

				_loader.MoveToArgumentOffset(line, 3);
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					scale1 = _loader.GetDoubleArgument();
				}
				_loader.MoveToArgumentOffset(line, 4);
				if (_loader.GetTokenType() == IfcTokenType::REF)
				{
					_loader.Reverse();
					Axis3 = GetCartesianPoint3D(_loader.GetRefArgument());
				}

				if (line.ifcType == ifc2x4::IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM)
				{
					_loader.MoveToArgumentOffset(line, 5);
					if (_loader.GetTokenType() == IfcTokenType::REF)
					{
						_loader.Reverse();
						scale2 = _loader.GetDoubleArgument();
					}

					_loader.MoveToArgumentOffset(line, 6);
					if (_loader.GetTokenType() == IfcTokenType::REF)
					{
						_loader.Reverse();
						scale3 = _loader.GetDoubleArgument();
					}
				}

				return glm::dmat4(
					glm::dvec4(Axis1 * scale1, 0),
					glm::dvec4(Axis2 * scale2, 0),
					glm::dvec4(Axis3 * scale3, 0),
					glm::dvec4(pos, 1)
				);
			}

			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "unexpected placement type", line.expressID, line.ifcType });
				break;
			}

			return glm::dmat4();
		}

		IfcTrimmingSelect ParseTrimSelect(std::vector<uint32_t>& tapeOffsets)
		{
			IfcTrimmingSelect ts;

			/*
			if (tapeOffsets.size() == 2)
			{
				_loader.MoveTo(tapeOffsets[0]);
				std::string type = _loader.GetStringArgument();

				if (type == "IFCPARAMETERVALUE")
				{
					ts.hasParam = true;
					_loader.MoveTo(tapeOffsets[1]);
					ts.param = _loader.GetDoubleArgument();
				}
				else
				{
					printf("Unsupported IfcTrimmingSelect type: IfcCartesianPoint");
				}
			}
			else if (tapeOffsets.size() == 1)
			{
				_loader.MoveTo(tapeOffsets[0]);
				uint32_t cartesianPointRef = _loader.GetRefArgument();

				ts.hasPos = true;
				// TODO: assuming cartesian point
				ts.pos = GetCartesianPoint2D(cartesianPointRef);
			}
			*/
			for (int i = 0; i < tapeOffsets.size(); i++)
			{
				_loader.MoveTo(tapeOffsets[i]);

				auto tokenType = _loader.GetTokenType();
				_loader.Reverse();

				if (tokenType == IfcTokenType::REF)
				{
					// caresian point
					uint32_t cartesianPointRef = _loader.GetRefArgument();
					ts.hasPos = true;
					ts.pos = GetCartesianPoint2D(cartesianPointRef);
				}
				else if (tokenType == IfcTokenType::LABEL)
				{
					// parametervalue
					std::string type = _loader.GetStringArgument();

					if (type == "IFCPARAMETERVALUE")
					{
						ts.hasParam = true;
						i++;
						_loader.MoveTo(tapeOffsets[i]);
						ts.param = _loader.GetDoubleArgument();
					}
				}
			}


			return ts;
		}

		template<uint32_t DIM>
		void ComputeCurve(uint32_t expressID, IfcCurve<DIM>& curve, int sameSense = -1, IfcTrimmingArguments trim = {})
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);
			switch (line.ifcType)
			{
			case ifc2x4::IFCPOLYLINE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto points = _loader.GetSetArgument();

				for (auto& token : points)
				{
					uint32_t pointId = _loader.GetRefArgument(token);
					curve.Add(GetCartesianPoint<DIM>(pointId));
				}

				break;
			}
			case ifc2x4::IFCCOMPOSITECURVE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto segments = _loader.GetSetArgument();
				auto selfIntersects = _loader.GetStringArgument();

				if (selfIntersects == "T")
				{
					// TODO: this is probably bad news
					_loader.ReportError({ LoaderErrorType::UNSPECIFIED, "Self intersecting composite curve", line.expressID });
				}

				for (auto& token : segments)
				{
					if (DEBUG_DUMP_SVG)
					{
						if constexpr (DIM == 2)
						{
							DumpSVGCurve(curve.points, L"partial_curve.html");
						}
					}

					uint32_t segmentId = _loader.GetRefArgument(token);

					ComputeCurve<DIM>(segmentId, curve, sameSense);
				}

				break;
			}
			case ifc2x4::IFCCOMPOSITECURVESEGMENT:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto transition = _loader.GetStringArgument();
				auto sameSenseS = _loader.GetStringArgument();
				auto parentID = _loader.GetRefArgument();

				bool sameSense = sameSenseS == "T";

				ComputeCurve<DIM>(parentID, curve, sameSense);

				break;
			}
			case ifc2x4::IFCLINE:
			{
				if constexpr (DIM == 2)
				{
					if (trim.start.hasPos && trim.end.hasPos)
					{
						curve.Add(trim.start.pos);
						curve.Add(trim.end.pos);
					}
					else
					{
						_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Unsupported trimmingselect IFCLINE", line.expressID, line.ifcType });
					}
				}
				else
				{
					_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Unsupported 3D curve IFCLINE", line.expressID, line.ifcType });
				}

				break;
			}
			case ifc2x4::IFCTRIMMEDCURVE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto basisCurveID = _loader.GetRefArgument();
				auto trim1Set = _loader.GetSetArgument();
				auto trim2Set = _loader.GetSetArgument();
				auto senseAgreementS = _loader.GetStringArgument();
				auto trimmingPreference = _loader.GetStringArgument();

				auto trim1 = ParseTrimSelect(trim1Set);
				auto trim2 = ParseTrimSelect(trim2Set);

				IfcTrimmingArguments trim;
				trim.exist = true;
				trim.start = trim1;
				trim.end = trim2;

				if (sameSense == 0)
				{
					std::swap(trim.end, trim.start);
				}

				bool senseAgreement = senseAgreementS == "T";

				ComputeCurve<DIM>(basisCurveID, curve, sameSense != -1 ? sameSense : senseAgreement, trim);

				break;
			}
			case ifc2x4::IFCINDEXEDPOLYCURVE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto pts2DRef = _loader.GetRefArgument();

				if (_loader.GetTokenType() != webifc::IfcTokenType::EMPTY)
				{
					_loader.ReportError({ LoaderErrorType::UNSPECIFIED, "non-empty segments in IFCINDEXEDPOLYCURVE currently not supported", line.expressID });
					break;
				}

				_loader.MoveToArgumentOffset(line, 2);

				if (_loader.GetTokenType() != webifc::IfcTokenType::EMPTY)
				{
					_loader.Reverse();
					auto selfIntersects = _loader.GetStringArgument();

					if (selfIntersects == "T")
					{
						// TODO: this is probably bad news
						_loader.ReportError({ LoaderErrorType::UNSPECIFIED, "Self intersecting ifcindexedpolycurve", line.expressID });
					}
				}

				if constexpr (DIM == 2)
				{
					auto pts = ReadIfcCartesianPointList2D(pts2DRef);
					for (auto& pt : pts)
					{
						curve.Add(pt);
					}
				}
				else
				{
					_loader.ReportError({ LoaderErrorType::UNSPECIFIED, "Parsing ifcindexedpolycurve in 3D is not possible", line.expressID });
				}

				break;
			}
			case ifc2x4::IFCCIRCLE:
			{
				_loader.MoveToArgumentOffset(line, 0);
				auto positionID = _loader.GetRefArgument();
				double radius = _loader.GetDoubleArgument();

				glm::mat<DIM + 1, DIM + 1, glm::f64, glm::defaultp> placement;
				if constexpr (DIM == 2)
				{
					placement = GetAxis2Placement2D(positionID);
				}
				else
				{
					placement = GetLocalPlacement(positionID);
				}

				double startDegrees = 0;
				double endDegrees = 360;

				if (trim.exist)
				{
					// TODO: support cartesian?
					startDegrees = trim.start.hasParam ? trim.start.param : 0;
					endDegrees = trim.end.hasParam ? trim.end.param : 360;
				}

				double startRad = startDegrees / 180 * CONST_PI;
				double endRad = endDegrees / 180 * CONST_PI;
				double lengthDegrees = endDegrees - startDegrees;

				// unset or true
				if (sameSense == 1 || sameSense == -1)
				{
					if (lengthDegrees < 0)
					{
						lengthDegrees += 360;
					}
				}
				else
				{
					if (lengthDegrees > 0)
					{
						lengthDegrees -= 360;
					}
				}

				double lengthRad = lengthDegrees / 180 * CONST_PI;


				size_t startIndex = curve.points.size();

				const int numSegments = _loader.GetSettings().CIRCLE_SEGMENTS_HIGH;

				for (int i = 0; i < numSegments; i++)
				{
					double ratio = static_cast<double>(i) / (numSegments - 1);
					double angle = startRad + ratio * lengthRad;

					glm::vec<DIM, glm::f64> pos;
					if constexpr (DIM == 2)
					{
						glm::dvec2 vec(0);
						vec[0] = radius * std::cos(angle);
						vec[1] = -radius * std::sin(angle); // not sure why we need this, but we apparently do
						pos = placement * glm::dvec3(vec, 1);
					}
					else
					{
						glm::dvec3 vec(0);
						vec[0] = radius * std::cos(angle);
						vec[1] = radius * std::sin(angle); // negative or not???
						pos = placement * glm::dvec4(glm::dvec3(vec), 1);
					}
					//glm::vec<DIM, glm::f64> pos = placement * glm::vec<DIM + 1, glm::f64>(vec, 1);
					curve.Add(pos);
				}

				// without a trim, we close the circle
				if (!trim.exist)
				{
					curve.Add(curve.points[startIndex]);
				}

				break;
			}

			default:
				_loader.ReportError({ LoaderErrorType::UNSUPPORTED_TYPE, "Unsupported curve type", line.expressID, line.ifcType });
				break;
			}

			if (DEBUG_DUMP_SVG)
			{
				if constexpr (DIM == 2)
				{
					DumpSVGCurve(curve.points, L"partial_curve.html");
				}
			}
		}

		glm::dvec2 GetCartesianPoint2D(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);
			auto coords = _loader.GetSetArgument();

			glm::dvec2 point(
				_loader.GetDoubleArgument(coords[0]),
				_loader.GetDoubleArgument(coords[1])
			);

			return point;
		}

		glm::dvec3 GetCartesianPoint3D(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			/*
			_loader.MoveToArgumentOffset(line, 0);
			auto coords = _loader.GetSetArgument();

			glm::dvec3 point1(
				_loader.GetDoubleArgument(coords[0]),
				_loader.GetDoubleArgument(coords[1]),
				_loader.GetDoubleArgument(coords[2])
			);
			*/

			// we should do the above, but its slow, so we do this:
			_loader.MoveToArgumentOffset(line, 0);
			IfcTokenType t = _loader.GetTokenType();

			// because these calls cannot be reordered we have to use intermediate variables
			double x = _loader.GetDoubleArgument();
			double y = _loader.GetDoubleArgument();
			double z = _loader.GetDoubleArgument();
			glm::dvec3 point(x, y, z);

			return point;
		}

		template<uint32_t DIM>
		glm::vec<DIM, glm::f64> GetCartesianPoint(uint32_t expressID)
		{
			uint32_t lineID = _loader.ExpressIDToLineID(expressID);
			auto& line = _loader.GetLine(lineID);

			_loader.MoveToArgumentOffset(line, 0);
			IfcTokenType t = _loader.GetTokenType();

			glm::vec<DIM, glm::f64> point;
			for (uint32_t i = 0; i < DIM; i++)
			{
				point[i] = _loader.GetDoubleArgument();
			}

			return point;
		}

		glm::dmat4 _coordinationMatrix = glm::dmat4(1.0);
		bool _isCoordinated = false;
		IfcLoader& _loader;
		std::unordered_map<uint32_t, IfcGeometry> _expressIDToGeometry;
		std::unordered_map<uint32_t, IfcComposedMesh> _expressIDToMesh;
	};
}