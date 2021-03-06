// Ariel: FLIP Fluid Simulator
// Written by Yining Karl Li
//
// File: flip.cpp
// Implements the FLIP sim

#include "flip.hpp"
#include "../math/kernels.inl"
#include "particlegridoperations.inl"
#include "particleresampler.inl"
#include "solver.inl"

namespace fluidCore{

FlipSim::FlipSim(const glm::vec3& maxres, const float& density, const float& stepsize, 
                 sceneCore::Scene* s, const bool& verbose){
    m_dimensions = maxres;  
    m_pgrid = new ParticleGrid(maxres);
    m_mgrid = CreateMacgrid(maxres);
    m_mgrid_previous = CreateMacgrid(maxres);
    m_max_density = 0.0f;
    m_density = density;
    m_scene = s;
    m_frame = 0;
    m_stepsize = stepsize;
    m_subcell = 1;
    m_picflipratio = .95f;
    m_densitythreshold = 0.04f;
    m_verbose = verbose;
}

FlipSim::~FlipSim(){
    delete m_pgrid;
    unsigned int particlecount = m_particles.size();
    for(unsigned int i=0; i<particlecount; i++){
        delete m_particles[i];
    }
    m_particles.clear();
    ClearMacgrid(m_mgrid);
}

void FlipSim::Init(){
    m_scene->BuildPermaSolidGeomLevelSet();
    //We need to figure out maximum particle pressure, 
    //so we generate a bunch of temporary particles
    //inside of a known area, sort them back onto the underlying grid, and calculate the density
    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    float h = m_density/maxd;
    //generate temp particles
    for(unsigned int i = 0; i < 10; i++){               //FOR_EACH_CELL
        for(unsigned int j = 0; j < 10; j++){ 
            for(unsigned int k = 0; k < 10; k++){ 
                Particle* p = new Particle;
                p->m_p = (glm::vec3(i,j,k) + glm::vec3(0.5f))*h;
                p->m_type = FLUID;
                p->m_mass = 1.0f;
                m_particles.push_back(p);
            }
        }
    }
    m_pgrid->Sort(m_particles);
    m_max_density = 1.0f;
    ComputeDensity(); 
    m_max_density = 0.0f;
    //sum densities across particles
    for(unsigned int n=0; n<m_particles.size(); n++) {
        Particle *p = m_particles[n];
        m_max_density = glm::max(m_max_density,p->m_density);
        delete p;
    }
    m_particles.clear();

    //Generate particles and sort
    m_scene->GenerateParticles(m_particles, m_dimensions, m_density, m_pgrid, 0);
    m_pgrid->Sort(m_particles);
    m_pgrid->MarkCellTypes(m_particles, m_mgrid.m_A, m_density);
}

void FlipSim::StoreTempParticleVelocities(){
    unsigned int particlecount = m_particles.size();

    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particlecount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int p=r.begin(); p!=r.end(); ++p){ 
                m_particles[p]->m_pt = m_particles[p]->m_p;
                m_particles[p]->m_ut = m_particles[p]->m_u;
            }
        }
    );
}

void FlipSim::Step(bool saveVDB, bool saveOBJ, bool savePARTIO){
    m_frame++;  
    std::cout << "Simulating Step: " << m_frame << "..." << std::endl;
    
    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);

    m_scene->GenerateParticles(m_particles, m_dimensions, m_density, m_pgrid, m_frame);
    m_scene->BuildSolidGeomLevelSet(m_frame);

    AdjustParticlesStuckInSolids();

    StoreTempParticleVelocities();
    m_pgrid->Sort(m_particles);
    ComputeDensity();
    ApplyExternalForces(); 
    SplatParticlesToMACGrid(m_pgrid, m_particles, &m_mgrid);
    m_pgrid->MarkCellTypes(m_particles, m_mgrid.m_A, m_density);
    StorePreviousGrid();
    EnforceBoundaryVelocity(&m_mgrid);
    Project();
    EnforceBoundaryVelocity(&m_mgrid);
    ExtrapolateVelocity();
    SubtractPreviousGrid();
    SolvePicFlip();
    AdvectParticles();
    
    CheckParticleSolidConstraints();
    StoreTempParticleVelocities();
    float h = m_density/maxd;
    ResampleParticles(m_pgrid, m_particles, m_scene, m_frame, m_stepsize, h, m_dimensions);

    CheckParticleSolidConstraints();

    if(saveVDB || saveOBJ || savePARTIO){
        m_scene->ExportParticles(m_particles, maxd, m_frame, saveVDB, saveOBJ, savePARTIO);
    }
}

void FlipSim::AdjustParticlesStuckInSolids(){
    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    unsigned int particleCount = m_particles.size();
    //pushi_back to vectors doesn't play nice with lambdas for some reason, so we have to
    //do something a little bit convoluted here...
    bool* particleInSolidChecks = new bool[particleCount];
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int p=r.begin(); p!=r.end(); ++p){ 
                particleInSolidChecks[p] = false;
                if(m_particles[p]->m_type==FLUID){
                                    m_particles[p]->m_temp = false;
                    m_particles[p]->m_temp2 = false;
                    glm::vec3 point = m_particles[p]->m_p * maxd;
                    unsigned int id;
                    if(m_scene->CheckPointInsideSolidGeom(point, m_frame, id)==true){
                        particleInSolidChecks[p] = true;
                    }
                }
            }
        }
    );
    //build vector of particles we need to adjust
    std::vector<Particle*> stuckParticles;
    stuckParticles.reserve(particleCount);  
    for(unsigned int p=0; p<particleCount; p++){
        if(particleInSolidChecks[p]==true){
            stuckParticles.push_back(m_particles[p]);
            m_particles[p]->m_pt = m_particles[p]->m_p;
        }
    }
    delete particleInSolidChecks;
    //figure out direction to nearest surface from levelset, then raycast for a precise result
    m_scene->GetSolidLevelSet()->ProjectPointsToSurface(stuckParticles, maxd);
    unsigned int stuckCount = stuckParticles.size();
    for(unsigned int p=0; p<stuckCount; p++){
        rayCore::Ray r;
        r.m_origin = stuckParticles[p]->m_pt * maxd;
        r.m_frame = m_frame;
        r.m_direction = glm::normalize(stuckParticles[p]->m_p - 
                                       stuckParticles[p]->m_pt);
        float d = glm::length(stuckParticles[p]->m_p - stuckParticles[p]->m_pt);
        float raynulltest = glm::length(r.m_direction);
        if(raynulltest==raynulltest){
            rayCore::Intersection hit = m_scene->IntersectSolidGeoms(r);
            float nearestDistance = glm::length(r.m_origin - hit.m_point);
            stuckParticles[p]->m_p = (r.m_origin + r.m_direction * 1.05f * nearestDistance)/maxd;
            stuckParticles[p]->m_u = glm::normalize(r.m_direction) * d;
        }
    }
    stuckParticles.clear();
}

void FlipSim::CheckParticleSolidConstraints(){
    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    unsigned int particlecount = m_particles.size();   
    // for(unsigned int p=0; p<particlecount; p++){ 
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particlecount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int p=r.begin(); p!=r.end(); ++p){ 
                if(m_particles[p]->m_type==FLUID){
                    rayCore::Ray r;
                    r.m_origin = m_particles[p]->m_pt * maxd;
                    r.m_frame = m_frame;
                    r.m_direction = glm::normalize(m_particles[p]->m_p - 
                                                   m_particles[p]->m_pt);
                    float d = glm::length(m_particles[p]->m_p - m_particles[p]->m_pt);
                    float raynulltest = glm::length(r.m_direction);

                    if(raynulltest==raynulltest){
                        rayCore::Intersection hit = m_scene->IntersectSolidGeoms(r);
                        float u_dir = glm::length(m_particles[p]->m_ut);
                        if(hit.m_hit==true){
                            float solidDistance = glm::length(r.m_origin - 
                                                              hit.m_point);
                            float velocityDistance = glm::length(m_particles[p]->m_p - 
                                                                 m_particles[p]->m_pt) * maxd;
                            if(solidDistance<velocityDistance){
                                m_particles[p]->m_p = (r.m_origin + 
                                                       r.m_direction * .90f * solidDistance)/maxd;
                                m_particles[p]->m_u = 2.0f*glm::dot(r.m_direction, hit.m_normal)*
                                                      hit.m_normal-glm::normalize(r.m_direction);
                                m_particles[p]->m_u = glm::normalize(m_particles[p]->m_u) * u_dir;
                            }
                        }    
                        r.m_origin = m_particles[p]->m_p * maxd;
                        unsigned int id;
                        if(m_scene->CheckPointInsideSolidGeom(r.m_origin, m_frame, id)==true){
                            m_particles[p]->m_u = -glm::normalize(r.m_direction) * u_dir;
                            m_particles[p]->m_p = m_particles[p]->m_pt + 
                                                  m_particles[p]->m_u * m_stepsize;
                        }
                    }
                }
            }
        }
    );
}

void FlipSim::AdvectParticles(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;
    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    unsigned int particleCount = m_particles.size();

    //update positions
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                if(m_particles[i]->m_type == FLUID){
                    glm::vec3 velocity = InterpolateVelocity(m_particles[i]->m_p, &m_mgrid);
                    m_particles[i]->m_p += m_stepsize*velocity;
                    //m_particles[i]->m_u = velocity;
                }
            }
        }
    );
    m_pgrid->Sort(m_particles); //sort

    //apply constraints for outer walls of sim
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int p0=r.begin(); p0!=r.end(); ++p0){  
                float r = 1.0f/maxd;
                if( m_particles[p0]->m_type == FLUID ) {
                    m_particles[p0]->m_p = glm::max(glm::vec3(r),glm::min(glm::vec3(1.0f-r),
                                                m_particles[p0]->m_p));
                }

                Particle* p = m_particles[p0];
                if(p->m_type == FLUID){
                    unsigned int i = glm::min(x-1.0f,p->m_p.x*maxd);
                    unsigned int j = glm::min(y-1.0f,p->m_p.y*maxd);
                    unsigned int k = glm::min(z-1.0f,p->m_p.z*maxd);            
                    std::vector<Particle*> neighbors = m_pgrid->GetCellNeighbors(glm::vec3(i,j,k), 
                                                                                 glm::vec3(1));
                    for(int p1=0; p1<neighbors.size(); p1++){
                        Particle* np = neighbors[p1];
                        float re = 1.5f*m_density/maxd;
                        if(np->m_type == SOLID){
                            float dist = glm::length(p->m_p-np->m_p); //check this later
                            if(dist<re){
                                glm::vec3 normal = np->m_n;
                                if(glm::length(normal)<0.0000001f && dist){
                                    normal = glm::normalize(p->m_p - np->m_p);
                                }
                                p->m_p += (re-dist)*normal;
                                p->m_u -= glm::dot(p->m_u, normal) * normal;
                            }
                        }
                    }
                }
            }
        }
    );
}

void FlipSim::SolvePicFlip(){
    int particleCount = m_particles.size();

    //store copy of current velocities for later
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                m_particles[i]->m_t = m_particles[i]->m_u;
            }
        }
    );

    SplatMACGridToParticles(m_particles, &m_mgrid_previous);

    //set FLIP velocity
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                m_particles[i]->m_t = m_particles[i]->m_u + m_particles[i]->m_t;
            }
        }
    );

    //set PIC velocity
    SplatMACGridToParticles(m_particles, &m_mgrid);

    //combine PIC and FLIP
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particleCount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                m_particles[i]->m_u = (1.0f-m_picflipratio)*m_particles[i]->m_u + 
                                      m_picflipratio*m_particles[i]->m_t;
            }
        }
    );
}

void FlipSim::StorePreviousGrid(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;
    //for every x face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x+1),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        m_mgrid_previous.m_u_x->SetCell(i,j,k,m_mgrid.m_u_x->GetCell(i,j,k));
                    }
                }
            }
        }
    );
    //for every y face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y+1; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        m_mgrid_previous.m_u_y->SetCell(i,j,k,m_mgrid.m_u_y->GetCell(i,j,k));
                    }
                }
            }
        }
    );
    //for every z face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z+1; ++k){
                        m_mgrid_previous.m_u_z->SetCell(i,j,k,m_mgrid.m_u_z->GetCell(i,j,k));
                    }
                }
            }
        }
    );
}

void FlipSim::SubtractPreviousGrid(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;
    //for every x face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x+1),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        float subx = m_mgrid.m_u_x->GetCell(i,j,k) - 
                                     m_mgrid_previous.m_u_x->GetCell(i,j,k);
                        m_mgrid_previous.m_u_x->SetCell(i,j,k,subx);
                    }
                }
            }
        }
    );
    //for every y face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y+1; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        float suby = m_mgrid.m_u_y->GetCell(i,j,k) - 
                                     m_mgrid_previous.m_u_y->GetCell(i,j,k);
                        m_mgrid_previous.m_u_y->SetCell(i,j,k,suby);
                    }
                }
            }
        }
    );
    //for every z face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){     
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z+1; ++k){
                        float subz = m_mgrid.m_u_z->GetCell(i,j,k) - 
                                     m_mgrid_previous.m_u_z->GetCell(i,j,k);
                        m_mgrid_previous.m_u_z->SetCell(i,j,k,subz);
                    }
                }
            }
        }
    );
}

void FlipSim::Project(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;

    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    float h = 1.0f/maxd; //cell width

    //compute divergence per cell
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){     
                for(unsigned int j = 0; j < y; ++j){
                    for(unsigned int k = 0; k < z; ++k){
                        float divergence = (m_mgrid.m_u_x->GetCell(i+1, j, k) - 
                                            m_mgrid.m_u_x->GetCell(i, j, k) + 
                                            m_mgrid.m_u_y->GetCell(i, j+1, k) - 
                                            m_mgrid.m_u_y->GetCell(i, j, k) +
                                            m_mgrid.m_u_z->GetCell(i, j, k+1) - 
                                            m_mgrid.m_u_z->GetCell(i, j, k)
                                            ) / h;
                        m_mgrid.m_D->SetCell(i,j,k,divergence);
                    }
                }
            }
        }
    );

    //compute internal level set for liquid surface
    m_pgrid->BuildSDF(m_mgrid, m_density);
    
    Solve(m_mgrid, m_subcell, m_verbose);

    if(m_verbose){
        std::cout << " " << std::endl;//TODO: no more stupid formatting hacks like this to std::out
    }

    //subtract pressure gradient
    SubtractPressureGradient();
}

void FlipSim::ExtrapolateVelocity(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;

    Grid<int>** mark = new Grid<int>*[3];
    Grid<int>** wallmark = new Grid<int>*[3];
    for(unsigned int i=0; i<3; i++){
        mark[i] = new Grid<int>(m_dimensions, 0);
        wallmark[i] = new Grid<int>(m_dimensions, 0);
    }

    //initalize temp grids with values
    //for every x face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x+1),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        mark[0]->SetCell(i,j,k, (i>0 && m_mgrid.m_A->GetCell(i-1,j,k)==FLUID) || 
                                                (i<x && m_mgrid.m_A->GetCell(i,j,k)==FLUID));
                        wallmark[0]->SetCell(i,j,k,(i<=0 || m_mgrid.m_A->GetCell(i-1,j,k)==SOLID) && 
                                                   (i>=x || m_mgrid.m_A->GetCell(i,j,k)==SOLID));
                    }
                }
            }
        }
    );
    //for every y face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){     
                for(unsigned int j = 0; j < y+1; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        mark[1]->SetCell(i,j,k, (j>0 && m_mgrid.m_A->GetCell(i,j-1,k)==FLUID) || 
                                                (j<y && m_mgrid.m_A->GetCell(i,j,k)==FLUID));
                        wallmark[1]->SetCell(i,j,k,(j<=0 || m_mgrid.m_A->GetCell(i,j-1,k)==SOLID) && 
                                                   (j>=y || m_mgrid.m_A->GetCell(i,j,k)==SOLID));
                    }
                }
            }
        }
    );
    //for every z face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){     
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z+1; ++k){
                        mark[2]->SetCell(i,j,k, (k>0 && m_mgrid.m_A->GetCell(i,j,k-1)==FLUID) || 
                                                (k<z && m_mgrid.m_A->GetCell(i,j,k)==FLUID));
                        wallmark[2]->SetCell(i,j,k,(k<=0 || m_mgrid.m_A->GetCell(i,j,k-1)==SOLID) && 
                                                   (k>=z || m_mgrid.m_A->GetCell(i,j,k)==SOLID));
                    }
                }
            }
        }
    );

    //extrapolate
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x+1),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y+1; ++j){ 
                    for(unsigned int k = 0; k < z+1; ++k){
                        for(unsigned int n=0; n<3; ++n){
                            if(n!=0 && i>x-1){ continue; };
                            if(n!=1 && j>y-1){ continue; };
                            if(n!=2 && k>z-1){ continue; };
                            if(!mark[n]->GetCell(i,j,k) && wallmark[n]->GetCell(i,j,k)){
                                unsigned int wsum = 0;
                                float sum = 0.0f;
                                glm::vec3 q[6] = { glm::vec3(i-1,j,k), glm::vec3(i+1,j,k), 
                                                   glm::vec3(i,j-1,k), glm::vec3(i,j+1,k), 
                                                   glm::vec3(i,j,k-1), glm::vec3(i,j,k+1) };
                                for(unsigned int qk=0; qk<6; ++qk){
                                    if(q[qk][0]>=0 && q[qk][0]<x+(n==0) && q[qk][1]>=0 && 
                                       q[qk][1]<y+(n==1) && q[qk][2]>=0 && q[qk][2]<z+(n==2) ) {
                                        if(mark[n]->GetCell(q[qk][0],q[qk][1],q[qk][2])){
                                            wsum ++;
                                            if(n==0){
                                                sum += m_mgrid.m_u_x->GetCell(q[qk][0],q[qk][1],
                                                                          q[qk][2]);
                                            }else if(n==1){
                                                sum += m_mgrid.m_u_y->GetCell(q[qk][0],q[qk][1],
                                                                          q[qk][2]);
                                            }else if(n==2){
                                                sum += m_mgrid.m_u_z->GetCell(q[qk][0],q[qk][1],
                                                                          q[qk][2]);
                                            }
                                        }
                                    }
                                }
                                if(wsum){
                                    if(n==0){
                                        m_mgrid.m_u_x->SetCell(i,j,k,sum/wsum);
                                    }else if(n==1){
                                        m_mgrid.m_u_y->SetCell(i,j,k,sum/wsum);
                                    }else if(n==2){
                                        m_mgrid.m_u_z->SetCell(i,j,k,sum/wsum);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    );
    for(unsigned int i=0; i<3; i++){
        delete mark[i];
        delete wallmark[i];
    }
    delete mark;
    delete wallmark;
}

void FlipSim::SubtractPressureGradient(){
    unsigned int x = (unsigned int)m_dimensions.x; unsigned int y = (unsigned int)m_dimensions.y; 
    unsigned int z = (unsigned int)m_dimensions.z;

    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);
    float h = 1.0f/maxd; //cell width

    //for every x face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x+1),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){ 
                    for(unsigned int k = 0; k < z; ++k){
                        if(i>0 && i<x){
                            float pf = m_mgrid.m_P->GetCell(i,j,k);
                            float pb = m_mgrid.m_P->GetCell(i-1,j,k);
                            if(m_subcell && m_mgrid.m_L->GetCell(i,j,k) * 
                               m_mgrid.m_L->GetCell(i-1,j,k) < 0.0f){
                                if(m_mgrid.m_L->GetCell(i,j,k)<0.0f){
                                    pf = m_mgrid.m_P->GetCell(i,j,k);
                                }else{
                                    pf = m_mgrid.m_L->GetCell(i,j,k)/
                                         glm::min(1.0e-3f,m_mgrid.m_L->GetCell(i-1,j,k))*
                                                  m_mgrid.m_P->GetCell(i-1,j,k);
                                }
                                if(m_mgrid.m_L->GetCell(i-1,j,k)<0.0f){
                                    pb = m_mgrid.m_P->GetCell(i-1,j,k);
                                }else{
                                    pb = m_mgrid.m_L->GetCell(i-1,j,k)/
                                         glm::min(1.0e-6f,m_mgrid.m_L->GetCell(i,j,k))*
                                                  m_mgrid.m_P->GetCell(i,j,k);
                                }               
                            }
                            float xval = m_mgrid.m_u_x->GetCell(i,j,k);
                            xval -= (pf-pb)/h;
                            m_mgrid.m_u_x->SetCell(i,j,k,xval);
                        }
                    }
                }
            } 
        }
    );
    //for every y face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y+1; ++j){
                    for(unsigned int k = 0; k < z; ++k){
                        if(j>0 && j<y){
                            float pf = m_mgrid.m_P->GetCell(i,j,k);
                            float pb = m_mgrid.m_P->GetCell(i,j-1,k);   
                            if(m_subcell && m_mgrid.m_L->GetCell(i,j,k) * 
                               m_mgrid.m_L->GetCell(i,j-1,k) < 0.0f){
                                if(m_mgrid.m_L->GetCell(i,j,k)<0.0f){
                                    pf = m_mgrid.m_P->GetCell(i,j,k);
                                }else{
                                    pf = m_mgrid.m_L->GetCell(i,j,k)/
                                         glm::min(1.0e-3f,m_mgrid.m_L->GetCell(i,j-1,k))*
                                                  m_mgrid.m_P->GetCell(i,j-1,k);
                                }
                                if(m_mgrid.m_L->GetCell(i,j-1,k)<0.0f){
                                    pb = m_mgrid.m_P->GetCell(i,j-1,k);
                                }else{
                                    pb = m_mgrid.m_L->GetCell(i,j-1,k)/
                                         glm::min(1.0e-6f,m_mgrid.m_L->GetCell(i,j,k))*
                                                  m_mgrid.m_P->GetCell(i,j,k);
                                }   
                            }
                            float yval = m_mgrid.m_u_y->GetCell(i,j,k);
                            yval -= (pf-pb)/h;
                            m_mgrid.m_u_y->SetCell(i,j,k,yval);
                        }       
                    } 
                }
            }
        }
    );
    //for every z face
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,x),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j = 0; j < y; ++j){
                    for(unsigned int k = 0; k < z+1; ++k){
                        if(k>0 && k<z){
                            float pf = m_mgrid.m_P->GetCell(i,j,k);
                            float pb = m_mgrid.m_P->GetCell(i,j,k-1);
                            if(m_subcell && m_mgrid.m_L->GetCell(i,j,k) * 
                               m_mgrid.m_L->GetCell(i,j,k-1) < 0.0f){
                                if(m_mgrid.m_L->GetCell(i,j,k)<0.0f){
                                    pf = m_mgrid.m_P->GetCell(i,j,k);
                                }else{
                                    pf = m_mgrid.m_L->GetCell(i,j,k)/
                                         glm::min(1.0e-3f,m_mgrid.m_L->GetCell(i,j,k-1))*
                                                  m_mgrid.m_P->GetCell(i,j,k-1);
                                }
                                if(m_mgrid.m_L->GetCell(i,j,k-1)<0.0f){
                                    pb = m_mgrid.m_P->GetCell(i,j,k-1);
                                }else{
                                    pb = m_mgrid.m_L->GetCell(i,j,k-1)/
                                         glm::min(1.0e-6f,m_mgrid.m_L->GetCell(i,j,k))*
                                                  m_mgrid.m_P->GetCell(i,j,k);
                                }   
                            }
                            float zval = m_mgrid.m_u_z->GetCell(i,j,k);
                            zval -= (pf-pb)/h;
                            m_mgrid.m_u_z->SetCell(i,j,k,zval);
                        }       
                    } 
                }
            }
        }
    );
}

void FlipSim::ApplyExternalForces(){
    std::vector<glm::vec3> externalForces = m_scene->GetExternalForces();
    unsigned int numberOfExternalForces = externalForces.size();
    unsigned int particlecount = m_particles.size();
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particlecount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                for(unsigned int j=0; j<numberOfExternalForces; j++){
                    m_particles[i]->m_u += externalForces[j]*m_stepsize;
                }
            }
        }
    );
}

void FlipSim::ComputeDensity(){

    float maxd = glm::max(glm::max(m_dimensions.x, m_dimensions.z), m_dimensions.y);

    unsigned int particlecount = m_particles.size();
    tbb::parallel_for(tbb::blocked_range<unsigned int>(0,particlecount),
        [=](const tbb::blocked_range<unsigned int>& r){
            for(unsigned int i=r.begin(); i!=r.end(); ++i){ 
                //Find neighbours
                if(m_particles[i]->m_type==SOLID){
                    m_particles[i]->m_density = 1.0f;
                }else{
                    glm::vec3 position = m_particles[i]->m_p;

                    position.x = (int)glm::max(0.0f,glm::min((int)maxd-1.0f,(int)maxd*position.x));
                    position.y = (int)glm::max(0.0f,glm::min((int)maxd-1.0f,(int)maxd*position.y));
                    position.z = (int)glm::max(0.0f,glm::min((int)maxd-1.0f,(int)maxd*position.z));
                    std::vector<Particle *> neighbors;
                    neighbors = m_pgrid->GetCellNeighbors(position, glm::vec3(1));
                    float weightsum = 0.0f;
                    unsigned int neighborscount = neighbors.size();
                    for(unsigned int m=0; m<neighborscount; m++){
                        // if(neighbors[m]->m_type!=SOLID){
                            float sqd = mathCore::Sqrlength(neighbors[m]->m_p, 
                                                            m_particles[i]->m_p);
                            //TODO: figure out a better density smooth approx than density/maxd
                            float weight = neighbors[m]->m_mass * 
                                           mathCore::Smooth(sqd, 4.0f*m_density/maxd);
                            weightsum = weightsum + weight;
                        // }
                    }
                    m_particles[i]->m_density = weightsum/m_max_density;
                }
            }
        }
    );
}

bool FlipSim::IsCellFluid(const int& x, const int& y, const int& z){
    if(m_scene->GetLiquidLevelSet()->GetCell(x,y,z)<0.0f){
        return true;
    }else{
        return false;
    }
}

std::vector<Particle*>* FlipSim::GetParticles(){
    return &m_particles;
}

glm::vec3 FlipSim::GetDimensions(){
    return m_dimensions;
}

sceneCore::Scene* FlipSim::GetScene(){
    return m_scene; 
}

FlipTask::FlipTask(FlipSim* sim, bool dumpVDB, bool dumpOBJ, bool dumpPARTIO){
    m_sim = sim;
    m_dumpPARTIO = dumpPARTIO;
    m_dumpOBJ = dumpOBJ;
    m_dumpVDB = dumpVDB;
}

tbb::task* FlipTask::execute(){
    m_sim->Step(m_dumpVDB, m_dumpOBJ, m_dumpPARTIO);
    return NULL;
}
}
