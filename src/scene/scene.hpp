// Ariel: FLIP Fluid Simulator
// Written by Yining Karl Li
//
// File: scene.hpp
// Scene class, meant for storing the scene state and checking particle positions

#ifndef SCENE_HPP
#define SCENE_HPP

#include <vector>
#include "../utilities/utilities.h"
#include "../grid/macgrid.inl"
#include "../geom/mesh.hpp"
#include "../grid/particlegrid.hpp"
#include "../grid/levelset.hpp"
#include "../spatial/bvh.hpp"

namespace sceneCore {
//====================================
// Class Declarations
//====================================

class Scene {
	friend class SceneLoader;
	public:
		Scene();
		~Scene();

		// void AddSolidObject(objCore::Obj* object, const int& startFrame, const int& endFrame);
		void GenerateParticles(std::vector<fluidCore::Particle*>& particles, 
							   const glm::vec3& dimensions, const float& density, 
							   fluidCore::ParticleGrid* pgrid, const int& frame);

		void AddExternalForce(glm::vec3 force);
		std::vector<glm::vec3>& GetExternalForces();

		fluidCore::LevelSet* GetSolidLevelSet();
		fluidCore::LevelSet* GetLiquidLevelSet();

		void BuildLevelSets(const int& frame);
		void SetPaths(const std::string& imagePath, const std::string& meshPath, 
					  const std::string& vdbPath, const std::string& partioPath);

		// void ProjectPointsToSolidSurface(std::vector<glm::vec3>& points);

		void ExportParticles(std::vector<fluidCore::Particle*> particles, const float& maxd, 
							 const int& frame, const bool& VDB, const bool& OBJ, 
							 const bool& PARTIO);

		//new stuff
		std::vector<geomCore::Geom*>& GetSolidGeoms();
		std::vector<geomCore::Geom*>& GetLiquidGeoms();

		rayCore::Intersection IntersectSolidGeoms(const rayCore::Ray& r);
		bool CheckPointInsideSolidGeom(const glm::vec3& p, const float& frame, 
									   unsigned int& solidGeomID);
		bool CheckPointInsideLiquidGeom(const glm::vec3& p, const float& frame, 
									    unsigned int& liquidGeomID);

		unsigned int GetLiquidParticleCount();

		std::string						m_imagePath;
		std::string						m_meshPath;
		std::string						m_vdbPath;
		std::string						m_partioPath;

	private:
		void AddParticle(const glm::vec3& pos, const geomtype& type, const float& thickness, 
						 const float& scale, std::vector<fluidCore::Particle*>& particles, 
						 const int& frame);

		std::vector< objCore::Obj* >		m_solidObjects;

		fluidCore::LevelSet*			m_solidLevelSet;
		fluidCore::LevelSet*			m_liquidLevelSet;

		std::vector<glm::vec3>			m_externalForces;

		//new stuff
		std::vector<geomCore::GeomTransform>						m_geomTransforms;
		std::vector<spaceCore::Bvh<objCore::Obj> >					m_meshFiles;
		std::vector<spaceCore::Bvh<objCore::InterpolatedObj> >		m_animMeshes;
		std::vector<geomCore::Geom>									m_geoms;
		std::vector<geomCore::MeshContainer>						m_meshContainers;
		std::vector<geomCore::AnimatedMeshContainer>				m_animmeshContainers;
		std::vector<geomCore::Geom*>								m_solids;
		std::vector<geomCore::Geom*>								m_liquids;

		bool 														m_highresSolidParticles;
		unsigned int 												m_solidParticlesIndexOffset;									
};
}

#endif
