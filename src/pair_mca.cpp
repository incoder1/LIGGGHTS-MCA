/* ----------------------------------------------------------------------
    This is the

    ██╗     ██╗ ██████╗  ██████╗  ██████╗ ██╗  ██╗████████╗███████╗
    ██║     ██║██╔════╝ ██╔════╝ ██╔════╝ ██║  ██║╚══██╔══╝██╔════╝
    ██║     ██║██║  ███╗██║  ███╗██║  ███╗███████║   ██║   ███████╗
    ██║     ██║██║   ██║██║   ██║██║   ██║██╔══██║   ██║   ╚════██║
    ███████╗██║╚██████╔╝╚██████╔╝╚██████╔╝██║  ██║   ██║   ███████║
    ╚══════╝╚═╝ ╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚══════╝®

    DEM simulation engine, released by
    DCS Computing Gmbh, Linz, Austria
    http://www.dcs-computing.com, office@dcs-computing.com

    LIGGGHTS® is part of CFDEM®project:
    http://www.liggghts.com | http://www.cfdem.com

    Core developer and main author:
    Christoph Kloss, christoph.kloss@dcs-computing.com

    LIGGGHTS® is open-source, distributed under the terms of the GNU Public
    License, version 2 or later. It is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. You should have
    received a copy of the GNU General Public License along with LIGGGHTS®.
    If not, see http://www.gnu.org/licenses . See also top-level README
    and LICENSE files.

    LIGGGHTS® and CFDEM® are registered trade marks of DCS Computing GmbH,
    the producer of the LIGGGHTS® software and the CFDEM®coupling software
    See http://www.cfdem.com/terms-trademark-policy for details.

-------------------------------------------------------------------------
    Contributing author and copyright for this file:

    Alexey Smolin (ISPMS SB RAS, Tomsk, Russia, http://www.ispms.ru)
    Nadia Salman (iT-CDT, Leeds, UK)

    Copyright 2016-     ISPMS SB RAS, Tomsk, Russia
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_mca.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "update.h"
#include "domain.h"
#include "neigh_list.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"
#include "neighbor.h"
#include "bond_mca.h"
#include "atom_vec_mca.h"
#include "vector_liggghts.h"
#include "rotations_mca.h"


using namespace LAMMPS_NS;
using namespace MathConst;
using namespace MCAAtomConst;

/// see also in 'fix_mca_meanstress.cpp'
///#define NO_MEANSTRESS
///#define NO_ROTATIONS
#define REAL_NULL_CONST 5.0E-22 // 5.0E-14

/* ---------------------------------------------------------------------- */

PairMCA::PairMCA(LAMMPS *lmp) : Pair(lmp)
{
  writedata = 1;
  single_enable = 0;
  Sy = NULL;
  Eh = NULL;
}

/* ---------------------------------------------------------------------- */

PairMCA::~PairMCA()
{
  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);

    memory->destroy(G);
    memory->destroy(K);
    memory->destroy(Sy);
    memory->destroy(Eh);
  }
}

/* ---------------------------------------------------------------------- */

inline void  PairMCA::compute_elastic_force()
{
  int i,j,k,jk,itype,jtype;
///  double xtmp,ytmp,ztmp,delx,dely,delz,r,r0;
///  double rsq,rinv;
///  double vxtmp,vytmp,vztmp;

  double **x = atom->x;
  double **v = atom->v;
  double **theta = atom->theta;
  double **theta_prev = atom->theta_prev;
  double **omega = atom->omega;
  const double * const mean_stress = atom->mean_stress;
  const double * const mean_stress_prev = atom->mean_stress_prev;
  const double mca_radius = atom->mca_radius;
  const int * const tag = atom->tag;
  const int * const type = atom->type;

  int **bond_atom = atom->bond_atom;
  const int * const num_bond = atom->num_bond;
  const int newton_bond = force->newton_bond;
  double ***bond_hist = atom->bond_hist;
  const double dtImpl = IMPLFACTOR*update->dt; /// TODO Allow to set in coeffs

  const int nlocal = atom->nlocal;
  const PairMCA * const mca_pair = (PairMCA*) force->pair;

//fprintf(logfile,"PairMCA::compute_elastic_force\n"); ///AS DEBUG TRACE

#if defined (_OPENMP)
#pragma omp parallel for private(i,j,k,jk,itype,jtype) shared(x,v,omega,theta,theta_prev,bond_atom,bond_hist) default(none) schedule(static)
#endif
  for (i = 0; i < nlocal; i++) {
    if (num_bond[i] == 0) continue;

    double rKHi,rKHj;// 1-2*G/(3*K) for atom i (j)
    double rHi,rHj;  // 2*G for atom i (j)
    double pi,pj;
    double d_p;
    double ei,d_e,d_e0;
    double rdSgmi,rdSgmj;
    double rGi,rGj;
    double qi,qj;
    double r,r0;

    itype = type[i];
    ///AS TODO make this property global as in 'fix_check_timestep_gran.cpp' :
    /// Y = static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulus","property/global","peratomtype",max_type,0,style));
    rGi = mca_pair->G[itype][itype];
    rHi = 2. * rGi;
    rKHi = mca_pair->K[itype][itype]; rKHi = 1. - rHi / (3. * rKHi);

    rdSgmi = rKHi*(mean_stress[i] - mean_stress_prev[i]); // rKHi*(arMS0[i]-arMS1[i]);
    for(k = 0; k < num_bond[i]; k++)
    {
      j = atom->map(bond_atom[i][k]);
      if (j == -1) {
        char str[512];
        sprintf(str,
                "Bond atoms %d %d missing at step " BIGINT_FORMAT,
                tag[i],bond_atom[i][k],update->ntimestep);
        error->one(FLERR,str);
      }
      j = domain->closest_image(i,j);
///if(tag[i]==10) fprintf(logfile,"PairMCA::compute_elastic_force bond_atom[%d][%d]=%d map()=%d \n",i,k,bond_atom[i][k],j);

      double *bond_hist_ik = &(bond_hist[i][k][0]);
      int found = 0;
      for(jk = 0; jk < num_bond[j]; jk++)
        if(bond_atom[j][jk] == tag[i]) {found = 1; break; }
      if (!found) error->all(FLERR,"PairMCA::compute_elastic_force 'jk' not found");
      double *bond_hist_jk = &(bond_hist[j][jk][0]);

      jtype = type[j];
      rGj = mca_pair->G[jtype][jtype];
      rHj = 2. * rGj;
      rKHj = mca_pair->K[jtype][jtype]; rKHj = 1. - rHj / (3. * rKHj);

      r = bond_hist_ik[R];
      r0 = bond_hist_ik[R_PREV];
      ei = bond_hist_ik[E];
      pi = bond_hist_ik[P_PREV];
      if (newton_bond) {
        //pj = bond_hist_ik[PJ_PREV];
        error->all(FLERR,"PairMCA::compute_elastic_force does not support 'newton on'");
      } else
        pj = bond_hist_jk[P_PREV];

      /// BEGIN central force
      d_e0 = (r - r0) / mca_radius;
      rdSgmj = rKHj*(mean_stress[j] - mean_stress_prev[j]);
      d_e  = (pj - pi + rHj*d_e0 + rdSgmj - rdSgmi) / (rHi + rHj);
      d_p = rHi*d_e + rdSgmi;
      ei += d_e;
      pi += d_p;
      /// END central force

      /// BEGIN shear force

      double *nv = &(bond_hist_ik[NX]);       // normal unit vector
      double *nv0 = &(bond_hist_ik[NX_PREV]); // normal unit vector at prev time step
      double dYij[3];
      vectorCross3D(nv0, nv, dYij); // rotaion of the pair
///if(i == 13) fprintf(logfile,"PairMCA::compute_elastic_force i=%d j=%d nv0= %20.12e %20.12e %20.12e nv= %20.12e %20.12e %20.12e \n",i,j,nv0[0],nv0[1],nv0[2],nv[0],nv[1],nv[2]); ///AS DEBUG
///if(i == 13) fprintf(logfile,"PairMCA::compute_elastic_force i=%d j=%d nv0xnv= %20.12e %20.12e %20.12e \n",i,j,dYij[0],dYij[1],dYij[2]); ///AS DEBUG

      double vdLij[3];
      vectorCopy3D(dYij, vdLij);
      vectorScalarMult3D(vdLij, r); // tangent displacement of the pair

      qi = mca_radius * (1. + bond_hist_ik[E]); // distance to the contact point from i
      qj = mca_radius * (1. + bond_hist_jk[E]); // distance to the contact point from j
      double vdTHi[3], vdTHj[3];
      double vR1[3], vR2[3], vRsum[3];
#ifdef NO_ROTATIONS
      vdTHi[0]=vdTHi[2]=vdTHi[2]=vdTHj[0]=vdTHj[1]=vdTHj[2]= 0.0;
#else
      vectorCopy3D(&(theta[i][0]), vR1);
      vectorCopy3D(&(theta_prev[i][0]), vR2);
      vectorFlip3D(vR2); //vectorScalarMult3D(vR2, -1.)
      SmallRotationSum(vR1, vR2, vdTHi); // rotation increment for i
      vectorCopy3D(&(theta[j][0]), vR1);
      vectorCopy3D(&(theta_prev[j][0]), vR2);
      vectorFlip3D(vR2); //vectorScalarMult3D(vR2, -1.)
      SmallRotationSum(vR1, vR2, vdTHj); // rotation increment for j
// implicit vvvv
      vectorCopy3D(&(omega[i][0]), vR2);
      vectorScalarMult3D(vR2, dtImpl);
      vectorCopy3D(vdTHi, vR1);
      SmallRotationSum(vR1, vR2, vdTHi);
      vectorCopy3D(&(omega[j][0]), vR2);
      vectorScalarMult3D(vR2, dtImpl);
      vectorCopy3D(vdTHj, vR1);
      SmallRotationSum(vR1, vR2, vdTHj);
// inmplicit ^^^^
      vectorCopy3D(vdTHi, vR1); vectorScalarMult3D(vR1, qi);
      vectorCopy3D(vdTHj, vR2); vectorScalarMult3D(vR2, qj);
      SmallRotationSum(vR1, vR2, vRsum);

      vectorCopy3D(nv, vR1);
      double proj = vectorDot3D(vRsum, nv); // projection of rotational part to the normal
      vectorScalarMult3D(vR1, proj);
      vectorSubtract3D(vRsum, vR1, vR2);    // tangential component of rotation
      vectorCopy3D(vdLij, vR1);
      vectorFlip3D(vR2);
      SmallRotationSum(vR1, vR2, vdLij);
#endif

      double rKS = 1. / (qj*rGi + qi*rGj);
      double rQR = qi * 0.5 * rKS;
      double vShear[3], vYi[3], vYj[3], vYij[3];
      vectorCopy3D(&(bond_hist_ik[SHX_PREV]), vShear);
      vectorCopy3D(&(bond_hist_ik[YX_PREV]), vYi);
      vectorCopy3D(&(bond_hist_jk[YX_PREV]), vYj);
      vectorCopy3D(vYj, vR1); vectorScalarMult3D(vR1, rQR);
      vectorCopy3D(vYi, vR2); vectorScalarMult3D(vR2, -rQR);
      vectorAdd3D(vR1, vR2, vYij);
      vectorScalarMult3D(vdLij, -rGj*rKS);
      SmallRotationSum(vdLij, vYij, dYij);
      vectorCopy3D(vShear, vR1);
      RotationSum(vR1, dYij, vShear);
      vectorScalarMult3D(vYi, 1./rHi);
      vectorCopy3D(vYi, vR1);
      RotationSum(vR1, dYij, vYi);
      vectorScalarMult3D(vYi, rHi);
      double vSij[3];
/* TODO      if(!azNbr0[iNbIndx].bL) {
        ///BEGIN correction due to sliding friction
        ...
        ///END correction due to sliding friction
        } else */
      {
        vectorCross3D(vYi, nv, vSij);
      }
      /// END shear force

      /// BEGIN bending-torsion torque
      double vdMij[3], vdMji[3], vMij[3];
#ifdef NO_ROTATIONS
vMij[0]= vMij[1]= vMij[2]=0.;
#else
      vectorCopy3D(vdTHi, vdMij);
      vectorCopy3D(vdTHj, vdMji);
      vectorScalarMult3D(vdMij, qi);
      vectorScalarMult3D(vdMji, -qj);
      RotationSum(vdMij, vdMji, vMij);
      vectorScalarMult3D(vMij, rGi*rGj/(rGi+rGj));
#endif
      /// END bending-torsion torque

      bond_hist_ik[E] = ei;
      bond_hist_ik[P] = pi;
      bond_hist_ik[SHX] = vShear[0];
      bond_hist_ik[SHY] = vShear[1];
      bond_hist_ik[SHZ] = vShear[2];
      bond_hist_ik[YX] = vYi[0];
      bond_hist_ik[YY] = vYi[1];
      bond_hist_ik[YZ] = vYi[2];
      bond_hist_ik[SX] = vSij[0];
      bond_hist_ik[SY] = vSij[1];
      bond_hist_ik[SZ] = vSij[2];
      bond_hist_ik[MX] = vMij[0];
      bond_hist_ik[MY] = vMij[1];
      bond_hist_ik[MZ] = vMij[2];
    }
  }
}

/* ---------------------------------------------------------------------- */

inline void  PairMCA::compute_equiv_stress()
{
  int i,k;
  double rdSgmi;

  const int * const num_bond = atom->num_bond;
  double ***bond_hist = atom->bond_hist;

  const int Nc = atom->coord_num;
  const int nlocal = atom->nlocal;

//fprintf(logfile,"PairMCA::compute_equiv_stress\n"); ///AS DEBUG TRACE
#if defined (_OPENMP)
#pragma omp parallel for private(i,k,rdSgmi) shared(bond_hist) default(none) schedule(static)
#endif
  for (i = 0; i < nlocal; i++) {
    double *theta;
    double *theta_prev;
    double *mean_stress = atom->mean_stress;
    double **bond_hist_i = &(bond_hist[i][0]);

    theta = &(atom->theta[i][0]);
    theta_prev = &(atom->theta_prev[i][0]);
    theta_prev[0] = theta[0];
    theta_prev[1] = theta[1];
    theta_prev[2] = theta[2];

#ifndef NO_MEANSTRESS
    if (num_bond[i] == 0) continue;

    rdSgmi = 0.0;
    for(k = 0; k < num_bond[i]; k++)
      rdSgmi += bond_hist_i[k][P];

    mean_stress[i] = rdSgmi / Nc;
#endif
  }

#if defined (_OPENMP)
#pragma omp parallel for private(i,k,rdSgmi) shared(bond_hist) default(none) schedule(static)
#endif
  for (i = 0; i < nlocal; i++) {
    if (num_bond[i] == 0) continue;

    double *equiv_stress = atom->equiv_stress;
    double *mean_stress = atom->mean_stress;
    rdSgmi = 0.0;
    for(k = 0; k < num_bond[i]; k++)
    {
      double *bond_hist_ik = &(bond_hist[i][k][0]);
      double xtmp,ytmp,ztmp,rStressInt;
      rStressInt = bond_hist_ik[P] - mean_stress[i];
      rStressInt = rStressInt*rStressInt;
      xtmp = bond_hist_ik[SX];
      ytmp = bond_hist_ik[SY];
      ztmp = bond_hist_ik[SZ];
      rStressInt += xtmp*xtmp + ytmp*ytmp + ztmp*ztmp;
      rdSgmi += rStressInt;
    }
    rdSgmi = sqrt(4.5*(rdSgmi) / Nc);
    equiv_stress[i] = rdSgmi;
  }
}

/* ---------------------------------------------------------------------- */

void PairMCA::correct_for_plasticity()
{
//fprintf(logfile, "PairMCA::correct_for_plasticity \n"); ///AS DEBUG TRACE
  int i,k;

  const int * const type = atom->type;
  const int * const tag = atom->tag;
  const int * const num_bond = atom->num_bond;
  int **bond_atom = atom->bond_atom;
  double ***bond_hist = atom->bond_hist;
  const PairMCA * const mca_pair = (PairMCA*) force->pair;

  const double * const mean_stress = atom->mean_stress;
  const double * const equiv_stress_prev = atom->equiv_stress_prev;

  const int newton_bond = force->newton_bond;
  const int Nc = atom->coord_num;
  const int nlocal = atom->nlocal;

#if defined (_OPENMP)
#pragma omp parallel for private(i,k) shared(bond_atom,bond_hist) default(none) schedule(static)
#endif
  for (i = 0; i < nlocal; i++) {
    if (num_bond[i] == 0) continue;

    double *equiv_stress = atom->equiv_stress;
    double *equiv_strain = atom->equiv_strain;
    int itype = type[i];
    double rGi = mca_pair->G[itype][itype];
    double r3Gi = 3.0*rGi;
    double rSgmInt = equiv_stress[i];
    double rdSgm = rSgmInt - equiv_stress_prev[i];
    double rSR = equiv_strain[i] + rdSgm/r3Gi;
    double rSyi =  mca_pair->Sy[itype][itype];
///!!!! Нужно ли нам заводить переменную для критической инт-ти деформаций ?
///Что будет критерием пластических свойств? Ведь один материал может быть пластическим, а другой - нет ?
    double rSR_Pla = rSyi>0. ? rSyi/r3Gi : 10.0*rSR; // equivalent yeild strain, if no plasticity make rSR_Pla > rSR
    equiv_strain[i] = rSR;

    if((rSyi>0.)&&(rSgmInt>REAL_NULL_CONST)&&(rdSgm>0.0)&&(rSR>rSR_Pla)) {
      double rEhi =  mca_pair->Eh[itype][itype];
//fprintf(logfile,"PairMCA::correct_for_plasticity rEhi[%d][%d]= %g \n",itype,itype,rEhi);
      double rSgmPl = rSyi + (rSR - rSR_Pla) * rEhi; // + work harderning
//fprintf(logfile,"PairMCA::correct_for_plasticity rSR= %g rSR_Pla= %g rSgmPl=%g\n",rSR,rSR_Pla,rSgmPl);
      double rM = rSgmPl/rSgmInt; // radial return factor
      double rMpli = 1.0 - rM;
      if(rMpli > REAL_NULL_CONST) {
        equiv_stress[i] = rSgmPl; // set according to the yeild surface
        double rP = (rSyi*rSyi*0.5/r3Gi + (rSyi + rSgmPl)*0.5*(rSR - rSR_Pla)) - rSgmPl*rSgmPl*0.5/r3Gi; // plastic heat
        ///TODO store plastic heat: if (rP > 0.0) arPH[i] = rP;
        for(k = 0; k < num_bond[i]; k++) {
          int j = atom->map(bond_atom[i][k]);
          if (j == -1) {
            char str[512];
            sprintf(str,
                "Bond atoms %d %d missing at step " BIGINT_FORMAT,
                tag[i],bond_atom[i][k],update->ntimestep);
            error->one(FLERR,str);
          }
          j = domain->closest_image(i,j);
///if(tag[i]==10) fprintf(logfile,"PairMCA::correct_for_plasticity bond_atom[%d][%d]=%d map()=%d \n",i,k,bond_atom[i][k],j);

          ///TODO if(Interact)
          {
            double *bond_hist_ik = &(bond_hist[i][k][0]);
            int found = 0;
            int jk;
            for(jk = 0; jk < num_bond[j]; jk++)
              if(bond_atom[j][jk] == tag[i]) {found = 1; break; }
            if (!found) error->all(FLERR,"PairMCA::correct_for_plasticity 'jk' not found");
            double *bond_hist_jk = &(bond_hist[j][jk][0]);

            rP = bond_hist_ik[P];
            rP = rP * rM + mean_stress[i] * rMpli;
            double rMij = rM;
            ///TODO if ((rP>0.0) && (!azNbr0[iNbIndx].bL)) {
            ///  rP = 0.0;
            ///  rMij = 0.0;
            ///}
            bond_hist_ik[P] = rP;
            bond_hist_ik[SX] *= rMij;
            bond_hist_ik[SY] *= rMij;
            bond_hist_ik[SZ] *= rMij;
            bond_hist_ik[YX] *= rMij;
            bond_hist_ik[YY] *= rMij;
            bond_hist_ik[YZ] *= rMij;
            bond_hist_ik[MX] *= rMij;
            bond_hist_ik[MY] *= rMij;
            bond_hist_ik[MZ] *= rMij;
          }
        }
      }
    }
  }
  return;
}

/* ---------------------------------------------------------------------- */

void PairMCA::compute_total_force(int eflag, int vflag)
{

  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = 0;

  const double mca_radius  = atom->mca_radius;
  const double contact_area  = atom->contact_area;
  const int * const tag = atom->tag; // tag of atom is their ID number
  double **x = atom->x;
  double **f = atom->f;
  double **torque = atom->torque;
  const double * const mean_stress  = atom->mean_stress;
  int **bondlist = neighbor->bondlist;
///AS I do not whant to use it, because it requires to be copied every step///  double **bondhistlist = neighbor->bondhistlist;
  double ***bond_hist = atom->bond_hist;
  const int * const num_bond = atom->num_bond;
  int **bond_atom = atom->bond_atom;

  const int nbondlist = neighbor->nbondlist;
  const int nlocal = atom->nlocal;
  const int newton_bond = force->newton_bond;
  const PairMCA * const mca_pair = (PairMCA*) force->pair;

//fprintf(logfile, "PairMCA::compute_total_force \n");  ///AS DEBUG TRACE

#if defined (_OPENMP)
#pragma omp parallel for shared(x,f,torque,bondlist,bond_atom,bond_hist) default(none) schedule(static)
#endif
  for (int n = 0; n < nbondlist; n++) {
    //1st check if bond is broken,
    if(bondlist[n][3])
    {
        fprintf(logfile,"PairMCA::compute_total_force bond %d has been already broken\n",n);
        continue;
    }

    int i1,i2,n1,n2;
    double rsq,r,rinv;
    double tor1,tor2,tor3;
    double delx,dely,delz;
    double dnforce[3],dtforce[3],nv[3];
    double dttorque[3];
    double A;
    double q1,q2;// distance to contact point

    i1 = bondlist[n][0];
    i2 = bondlist[n][1];

    for (n1 = 0; n1 < num_bond[i1]; n1++) {
      if (bond_atom[i1][n1]==tag[i2]) break;
    }
    if (n1 == num_bond[i1]) error->all(FLERR,"Internal error in PairMCA: n1 not found");

    for (n2 = 0; n2 < num_bond[i2]; n2++) {
      if (bond_atom[i2][n2]==tag[i1]) break;
    }
    if (n2 == num_bond[i2]) error->all(FLERR,"Internal error in PairMCA: n2 not found");

    double *bond_hist1 = &(bond_hist[i1][n1][0]);
    double *bond_hist2 = &(bond_hist[i2][n2][0]);

    double pi = bond_hist1[P];
    double pj = bond_hist2[P];
    if ( bondlist[n][3] && ((pi>0.)||(pj>0.)) ) {
      error->warning(FLERR,"PairMCA::compute_total_force (pi>0.)||(pj>0.) - be careful!");
      continue;
    }

    q1 = mca_radius*(1. + bond_hist1[E]);
    q2 = mca_radius*(1. + bond_hist2[E]);

    //int type = bondlist[n][2];

    double rD0 = 2.0*mca_radius;
    double rDij = bond_hist1[R];
    int itype = atom->type[i1];
    double rKi =  mca_pair->K[itype][itype];
    int jtype = atom->type[i2];
    double rKj =  mca_pair->K[jtype][jtype];
    double rDStress = 0.5 * (mean_stress[i1]/rKi + mean_stress[i2]/rKj);
    A = contact_area * (1. + rDStress) * rD0 / rDij; // contact area updated for pyramid

    //bond_hist1[A] = A;
    //bond_hist2[A] = A;

    delx = x[i1][0] - x[i2][0];
    dely = x[i1][1] - x[i2][1];
    delz = x[i1][2] - x[i2][2];

    rsq = delx*delx + dely*dely + delz*delz;
    r = sqrt(rsq);
    rinv = -1. / r; // "-" means that unit vector is from i1 to i2

    // normal unit vector
    nv[0] = delx*rinv; // compute normal here because in bond_hist1[NX] it has implicit increment
    nv[1] = dely*rinv;
    nv[2] = delz*rinv;

    // normal force
    pi += pj; pi *= 0.5;
    vectorScalarMult3D(nv, pi, dnforce);

    // tangential force
    dtforce[0] = 0.5 * (bond_hist2[SX] - bond_hist1[SX]);
    dtforce[1] = 0.5 * (bond_hist2[SY] - bond_hist1[SY]);
    dtforce[2] = 0.5 * (bond_hist2[SZ] - bond_hist1[SZ]);

    // torque due to tangential force
    vectorCross3D(nv, dtforce, dttorque);

    // torque due to torsion and bending
#ifdef NO_ROTATIONS
    tor1 = tor2 = tor3 = 0.;
#else
    tor1 = 0.5 * A * (bond_hist2[MX] - bond_hist1[MX]);
    tor2 = 0.5 * A * (bond_hist2[MY] - bond_hist1[MY]);
    tor3 = 0.5 * A * (bond_hist2[MZ] - bond_hist1[MZ]);
#endif

    // energy
    //if (eflag) error->all(FLERR,"MCA bonds currently do not support energy calculation");

    // apply force to each of 2 atoms

    if (newton_bond || i1 < nlocal) {
      f[i1][0] += (dnforce[0] + dtforce[0]) * A;
      f[i1][1] += (dnforce[1] + dtforce[1]) * A;
      f[i1][2] += (dnforce[2] + dtforce[2]) * A;
      torque[i1][0] += q1 * A * dttorque[0] + tor1;
      torque[i1][1] += q1 * A * dttorque[1] + tor2;
      torque[i1][2] += q1 * A * dttorque[2] + tor3;
    }

    if (newton_bond || i2 < nlocal) {
      f[i2][0] -= (dnforce[0] + dtforce[0]) * A;
      f[i2][1] -= (dnforce[1] + dtforce[1]) * A;
      f[i2][2] -= (dnforce[2] + dtforce[2]) * A;
      torque[i2][0] += q2 * A * dttorque[0] - tor1;
      torque[i2][1] += q2 * A * dttorque[1] - tor2;
      torque[i2][2] += q2 * A * dttorque[2] - tor3;
    }

    //if (evflag) ev_tally(i1,i2,nlocal,newton_bond,ebond,0./*fbond*/,delx,dely,delz);
  }
}

/* ---------------------------------------------------------------------- */

void PairMCA::compute(int eflag, int vflag)
{

  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;

//fprintf(logfile,"PairMCA::compute\n"); ///AS DEBUG TRACE
///  swap_prev(); Moved to fixMCAExchangeMeanStress::post_integrate()
///  predict_mean_stress(); Moved to fixMCAExchangeMeanStress::post_integrate()
  compute_elastic_force();
  compute_equiv_stress();
  correct_for_plasticity();
  compute_total_force(eflag,vflag);

  if (vflag_fdotr) virial_fdotr_compute();
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairMCA::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  memory->create(setflag,n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  memory->create(cutsq,n+1,n+1,"pair:cutsq");

  memory->create(G,n+1,n+1,"pair:G");
  memory->create(K,n+1,n+1,"pair:K");
  memory->create(Sy,n+1,n+1,"pair:Sy");
  memory->create(Eh,n+1,n+1,"pair:Eh");
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairMCA::settings(int narg, char **arg)
{
/*
  if (narg != 1) error->all(FLERR,"Illegal pair_style command");

  cut_global = force->numeric(FLERR,arg[0]); ///AS Later we will change it

  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
        if (setflag[i][j]) cut[i][j] = cut_global;
  }*/
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairMCA::coeff(int narg, char **arg)
{
  if (narg < 4)
    error->all(FLERR,"Incorrect args for mca pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double G_one = force->numeric(FLERR,arg[2]);
  double K_one = force->numeric(FLERR,arg[3]);

  double Sy_one = -1.0; // no plasticity
  double Eh_one = 0.0; // no harderning
  if (narg > 4) {
    Sy_one = force->numeric(FLERR,arg[4]);
    if (narg == 6) Eh_one = force->numeric(FLERR,arg[5]);
    if (narg > 6) error->all(FLERR,"Incorrect args for mca pair coefficients for plasticity");
  }

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      G[i][j] = G_one;
      K[i][j] = K_one;
      Sy[i][j] = Sy_one;
      Eh[i][j] = Eh_one;
      setflag[i][j] = 1; // 0/1 = whether each i,j has been set
      count++;
    }
  }

  if (count == 0) error->all(FLERR,"Incorrect args for mca pair coefficients");
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairMCA::init_one(int i, int j)
{
  // always mix Gs geometrically

  if (setflag[i][j] == 0) {
    G[i][j] = (G[i][i] * G[j][j]) / (G[i][i] + G[j][j]);
    K[i][j] = (K[i][i] * K[j][j]) / (K[i][i] + K[j][j]);
    Sy[i][j] = (Sy[i][i] * Sy[j][j]) / (Sy[i][i] + Sy[j][j]);
    Eh[i][j] = (Eh[i][i] * Eh[j][j]) / (Eh[i][i] + Eh[j][j]);
  }

  G[j][i] = G[i][j];
  K[j][i] = K[i][j];
  Sy[j][i] = Sy[i][j];
  Eh[j][i] = Eh[i][j];

  return 1.2*atom->mca_radius; // have to return cut_global
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairMCA::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
        fwrite(&G[i][j],sizeof(double),1,fp);
        fwrite(&K[i][j],sizeof(double),1,fp);
        fwrite(&Sy[i][j],sizeof(double),1,fp);
        fwrite(&Eh[i][j],sizeof(double),1,fp);
      }
    }
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairMCA::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
        if (me == 0) {
          fread(&G[i][j],sizeof(double),1,fp);
          fread(&K[i][j],sizeof(double),1,fp);
          fread(&Sy[i][j],sizeof(double),1,fp);
          fread(&Eh[i][j],sizeof(double),1,fp);
        }
        MPI_Bcast(&G[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&K[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&Sy[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&Eh[i][j],1,MPI_DOUBLE,0,world);
      }
    }
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairMCA::write_restart_settings(FILE *fp)
{
  fwrite(&cut_global,sizeof(double),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairMCA::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_global,sizeof(double),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ----------------------------------------------------------------------
   proc 0 writes to data file
------------------------------------------------------------------------- */

void PairMCA::write_data(FILE *fp)
{
  for (int i = 1; i <= atom->ntypes; i++)
    fprintf(fp,"%d %g %g %g %g\n",i,G[i][i],K[i][i],Sy[i][i],Eh[i][i]);
}

/* ----------------------------------------------------------------------
   proc 0 writes all pairs to data file
------------------------------------------------------------------------- */

void PairMCA::write_data_all(FILE *fp)
{
  for (int i = 1; i <= atom->ntypes; i++)
    for (int j = i; j <= atom->ntypes; j++)
      fprintf(fp,"%d %d %g %g %g %g\n",i,j,G[i][j],K[i][j],Sy[i][j],Eh[i][j]);
}
