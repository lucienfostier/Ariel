// Kai: FLIP Fluid Simulator
// Written by Yining Karl Li
//
// File: solver.inl
// Breakout file for PCG solver

#ifndef SOLVER_INL
#define SOLVER_INL

#include "../grid/macgrid.inl"
#include "../grid/particlegrid.hpp"
#include "../grid/levelset.hpp"
#include "../utilities/utilities.h"
#include "../grid/gridutils.inl"
#include <omp.h>

using namespace std;
using namespace glm;

namespace fluidCore {
//====================================
// Struct and Function Declarations
//====================================

//TODO: const some things man!
//Forward declarations for externed inlineable methods
extern inline void solve(macgrid& mgrid, const int& subcell);
extern inline void flipDivergence(macgrid& mgrid);
extern inline void buildPreconditioner(floatgrid* pc, macgrid& mgrid, const int& subcell);
extern inline float fluidRef(intgrid* A, int i, int j, int k, int qi, int qj, int qk, vec3 dimensions);
extern inline float preconditionerRef(floatgrid* p, int i, int j, int k, vec3 dimensions);
extern inline float fluidDiag(intgrid* A, floatgrid* L, int i, int j, int k, vec3 dimensions, int subcell);
extern inline void solveConjugateGradient(macgrid& mgrid, floatgrid* pc, const int& subcell);
extern inline void computeAx(intgrid* A, floatgrid* L, floatgrid* X, floatgrid* target, 
							 vec3 dimensions, int subcell);
extern inline float xRef(intgrid* A, floatgrid* L, floatgrid* X, vec3 f, vec3 p, vec3 dimensions, 
						 int subcell);
extern inline void op(intgrid* A, floatgrid* X, floatgrid* Y, floatgrid* target, float alpha, 
					  vec3 dimensions);

//====================================
// Function Implementations
//====================================

//Takes a grid, multiplies everything by -1
void flipGrid(floatgrid* grid, vec3 dimensions){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	// #pragma omp parallel for
	for( int gn=0; gn<x*y*z; gn++ ) { 
		int i=(gn%((x)*(y)))%(z); int j=(gn%((x)*(y)))/(z); int k = gn/((x)*(y)); 
		float flipped = -grid->getCell(i,j,k);
		grid->setCell(i,j,k,flipped);
	}
}

//Helper for preconditioner builder
float ARef(intgrid* A, int i, int j, int k, int qi, int qj, int qk, vec3 dimensions){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	if( i<0 || i>x-1 || j<0 || j>y-1 || k<0 || k>z-1 || A->getCell(i,j,k)!=FLUID ){ //if not liquid
		return 0.0;
	} 
	if( qi<0 || qi>x-1 || qj<0 || qj>y-1 || qk<0 || qk>z-1 || A->getCell(qi,qj,qk)!=FLUID ){ //if not liquid
		return 0.0;
	} 
	return -1.0;	
}

//Helper for preconditioner builder
float PRef(floatgrid* p, int i, int j, int k, vec3 dimensions){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	if( i<0 || i>x-1 || j<0 || j>y-1 || k<0 || k>z-1 || p->getCell(i,j,k)!=FLUID ){ //if not liquid
		return 0.0f;
	} 
	return p->getCell(i,j,k);
}

//Helper for preconditioner builder
float ADiag(intgrid* A, floatgrid* L, int i, int j, int k, vec3 dimensions, int subcell){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	float diag = 6.0;
	if( A->getCell(i,j,k) != FLUID ) return diag;
	int q[][3] = { {i-1,j,k}, {i+1,j,k}, {i,j-1,k}, {i,j+1,k}, {i,j,k-1}, {i,j,k+1} };
	for( int m=0; m<6; m++ ) {
		int qi = q[m][0];
		int qj = q[m][1];
		int qk = q[m][2];
		if( qi<0 || qi>x-1 || qj<0 || qj>y-1 || qk<0 || qk>z-1 || A->getCell(qi,qj,qk)==SOLID ){
			diag -= 1.0;
		}
		else if( A->getCell(qi,qj,qk)==AIR && subcell ) {
			diag -= L->getCell(qi,qj,qk)/glm::min(1.0e-6f,L->getCell(i,j,k));
		}
	}
	
	return diag;
}

//Does what it says
void buildPreconditioner(floatgrid* pc, macgrid& mgrid, const int& subcell){
	int x = (int)mgrid.dimensions.x; int y = (int)mgrid.dimensions.y; int z = (int)mgrid.dimensions.z;
	float a = 0.25f;
	//for now run single threaded, multithreaded seems to cause VDB write issues here
	// #pragma omp parallel for
	for( int gn=0; gn<x*y*z; gn++ ) { 
		int i=(gn%((x)*(y)))%(z); int j=(gn%((x)*(y)))/(z); int k = gn/((x)*(y)); 
		if(mgrid.A->getCell(i,j,k)==FLUID){	

			float left = ARef(mgrid.A,i-1,j,k,i,j,k,mgrid.dimensions) * PRef(pc,i-1,j,k,mgrid.dimensions);
			float bottom = ARef(mgrid.A,i,j-1,k,i,j,k,mgrid.dimensions) * PRef(pc,i,j-1,k,mgrid.dimensions);
			float back = ARef(mgrid.A,i,j,k-1,i,j,k,mgrid.dimensions) * PRef(pc,i,j,k-1,mgrid.dimensions);
			float diag = ADiag(mgrid.A, mgrid.L,i,j,k,mgrid.dimensions,subcell);
			float e = diag - (left*left) - (bottom*bottom) - (back*back);
			if(diag>0){
				if( e < a*diag ){
					e = diag;
				}
				pc->setCell(i,j,k, 1.0f/sqrt(e));
			}
		}
	}
}

//Helper for PCG solver: read X with clamped bounds
float xRef(intgrid* A, floatgrid* L, floatgrid* X, vec3 f, vec3 p, vec3 dimensions, int subcell){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	int i = glm::min(glm::max(0,(int)p.x),x-1); int fi = (int)f.x;
	int j = glm::min(glm::max(0,(int)p.y),y-1); int fj = (int)f.y;
	int k = glm::min(glm::max(0,(int)p.z),z-1); int fk = (int)f.z;
	if(A->getCell(i,j,k) == FLUID){
		return X->getCell(i,j,k);
	}else if(A->getCell(i,j,k) == SOLID){
		return X->getCell(fi,fj,fk);
	} 
	if(subcell){
		return L->getCell(i,j,k)/glm::min(1.0e-6f,L->getCell(fi,fj,fk))*X->getCell(fi,fj,fk);
	}else{
		return 0.0f;
	}
}

// target = X + alpha*Y
void op(intgrid* A, floatgrid* X, floatgrid* Y, floatgrid* target, float alpha, vec3 dimensions){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	for( int gn=0; gn<x*y*z; gn++ ) { 
		int i=(gn%((x)*(y)))%(z); int j=(gn%((x)*(y)))/(z); int k = gn/((x)*(y)); 
		if(A->getCell(i,j,k)==FLUID){
			float targetval = X->getCell(i,j,k)+alpha*Y->getCell(i,j,k);
			target->setCell(i,j,k,targetval);
		}else{
			target->setCell(i,j,k,0.0f);
		}
	}
}

//Helper for PCG solver: target = AX
void computeAx(intgrid* A, floatgrid* L, floatgrid* X, floatgrid* target, vec3 dimensions, int subcell){
	int x = (int)dimensions.x; int y = (int)dimensions.y; int z = (int)dimensions.z;
	float n = (float)glm::max(glm::max(x,y),z);
	float h = 1.0f/(n*n);
	for( int gn=0; gn<x*y*z; gn++ ) { 
		int i=(gn%((x)*(y)))%(z); int j=(gn%((x)*(y)))/(z); int k = gn/((x)*(y)); 

		if(A->getCell(i,j,k) == FLUID){
			float result = (6.0f*X->getCell(i,j,k)
							-xRef(A, L, X, vec3(i,j,k), vec3(i+1,j,k), dimensions, subcell)
							-xRef(A, L, X, vec3(i,j,k), vec3(i-1,j,k), dimensions, subcell)
							-xRef(A, L, X, vec3(i,j,k), vec3(i,j+1,k), dimensions, subcell)
							-xRef(A, L, X, vec3(i,j,k), vec3(i,j-1,k), dimensions, subcell)
							-xRef(A, L, X, vec3(i,j,k), vec3(i,j,k+1), dimensions, subcell)
							-xRef(A, L, X, vec3(i,j,k), vec3(i,j,k-1), dimensions, subcell))/h;

			target->setCell(i,j,k,result);
		} else {
			target->setCell(i,j,k,0.0f);
		}
	}
}

//Does what it says
void solveConjugateGradient(macgrid& mgrid, floatgrid* pc, const int& subcell){
	floatgrid* r = new floatgrid(0.0f);
	floatgrid* z = new floatgrid(0.0f);
	floatgrid* s = new floatgrid(0.0f);

	computeAx(mgrid.A, mgrid.L, mgrid.P, z, mgrid.dimensions, subcell);	// z = apply A(x)
	op(mgrid.A, mgrid.D, z, r, -1.0f, mgrid.dimensions);                // r = b-Ax
}

void solve(macgrid& mgrid, const int& subcell){
	//flip divergence
	cout << "Flipping divergence..." << endl;
	flipGrid(mgrid.D, mgrid.dimensions);

	//build preconditioner
	cout << "Building preconditioner matrix..." << endl;
	floatgrid* preconditioner = new floatgrid(0.0f);
	buildPreconditioner(preconditioner, mgrid, subcell);

	//solve conjugate gradient
	cout << "Solving Conjugate Gradient..." << endl;
	solveConjugateGradient(mgrid, preconditioner, subcell);

	delete preconditioner;
}

}

#endif