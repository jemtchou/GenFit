/* Copyright 2008-2014, Technische Universitaet Muenchen,
   Authors: Christian Hoeppner & Sebastian Neubert

   This file is part of GENFIT.

   GENFIT is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published
   by the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GENFIT is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with GENFIT.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "MaterialEffects.h"
#include "Exception.h"
#include "IO.h"

#include <stdexcept>
#include <string>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include <TDatabasePDG.h>
#include <TMath.h>

#include <TH1D.h>
#include <TFile.h>


namespace genfit {

MaterialEffects* MaterialEffects::instance_ = nullptr;


MaterialEffects::MaterialEffects():
  noEffects_(false),
  energyLossBetheBloch_(true), noiseBetheBloch_(true),
  noiseCoulomb_(true),
  energyLossBrems_(true), noiseBrems_(true),
  ignoreBoundariesBetweenEqualMaterials_(true),
  stepSize_(0),
  dEdx_(0),
  E_(0),
  matDensity_(0),
  matZ_(0),
  matA_(0),
  radiationLength_(0),
  mEE_(0),
  mscModelCode_(0),
  materialInterface_(nullptr),
  debugLvl_(0)
{
}

MaterialEffects::~MaterialEffects()
{
  if (materialInterface_ != nullptr) delete materialInterface_;
}

MaterialEffects* MaterialEffects::getInstance()
{
  if (instance_ == nullptr) instance_ = new MaterialEffects();
  return instance_;
}

void MaterialEffects::destruct()
{
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

void MaterialEffects::init(AbsMaterialInterface* matIfc)
{
  if (materialInterface_ != nullptr) {
    std::string msg("MaterialEffects::initMaterialInterface(): Already initialized! ");
    std::runtime_error err(msg);
  }
  materialInterface_ = matIfc;
}



void MaterialEffects::setMscModel(const std::string& modelName)
{
  if (modelName == std::string("GEANE")) {
    mscModelCode_ = 0;
  } else if (modelName == std::string("Highland")) {
    mscModelCode_ = 1;
  } else {// throw exception
    std::string errorMsg = std::string("There is no MSC model called \"") + modelName + "\". Maybe it is not implemented or you misspelled the model name";
    Exception exc(errorMsg, __LINE__, __FILE__);
    exc.setFatal();
    errorOut << exc.what();
    throw exc;
  }
}


Scalar MaterialEffects::effects(const std::vector<RKStep>& steps,
                                int materialsFXStart,
                                int materialsFXStop,
                                const Scalar& mom,
                                const int& pdg,
                                M7x7* noise)
{

  if (debugLvl_ > 0) {
    debugOut << "     MaterialEffects::effects \n";
  }

  /*debugOut << "noEffects_ " << noEffects_ << "\n";
  debugOut << "energyLossBetheBloch_ " << energyLossBetheBloch_ << "\n";
  debugOut << "noiseBetheBloch_ " << noiseBetheBloch_ << "\n";
  debugOut << "noiseCoulomb_ " << noiseCoulomb_ << "\n";
  debugOut << "energyLossBrems_ " << energyLossBrems_ << "\n";
  debugOut << "noiseBrems_ " << noiseBrems_ << "\n";*/


  if (noEffects_) return 0.;

  if (materialInterface_ == nullptr) {
    std::string msg("MaterialEffects hasn't been initialized with a correct AbsMaterialInterface pointer!");
    std::runtime_error err(msg);
    throw err;
  }

  bool doNoise(noise != nullptr);

  Scalar momLoss = 0.;
  const Scalar mass = getParticleMass(pdg);

  for ( std::vector<RKStep>::const_iterator it = steps.begin() + materialsFXStart; it !=  steps.begin() + materialsFXStop; ++it) { // loop over steps

    Scalar realPath = it->matStep_.stepSize_;
    if (fabs(realPath) < 1.E-8) {
      // do material effects only if distance is not too small
      continue;
    }

    if (debugLvl_ > 0) {
      debugOut << "     calculate matFX ";
      if (doNoise) 
        debugOut << "and noise";
      debugOut << " for ";
      debugOut << "stepSize = " << it->matStep_.stepSize_ << "\t";
      it->matStep_.materialProperties_.Print();
    }

    Scalar stepSign(1.);
    if (realPath < 0)
      stepSign = -1.;
    realPath = fabs(realPath);
    stepSize_ = realPath;

    it->matStep_.materialProperties_.getMaterialProperties(matDensity_, matZ_, matA_, radiationLength_, mEE_);

    if (matZ_ > 1.E-3) { // don't calculate energy loss for vacuum

      momLoss += momentumLoss(stepSign, mom - momLoss, false, pdg);

      if (doNoise){

        if (E_ <= mass) {
          Exception exc("MaterialEffects::effects - Energy <= mass",__LINE__,__FILE__);
          exc.setFatal();
          throw exc;
        }

        const Scalar gamma = E_ / mass;
        const Scalar gammaSquare = gamma * gamma;
        const Scalar betaSquare = 1 - 1 / gammaSquare;
        const Scalar p = E_ * sqrt(betaSquare);
        const Scalar pSquare = p*p;

        if (energyLossBetheBloch_ && noiseBetheBloch_)
          this->noiseBetheBloch(*noise, p, betaSquare, gamma, gammaSquare, pdg);

        if (noiseCoulomb_)
          this->noiseCoulomb(*noise, *((M1x3*) &it->state7_[3]), pSquare, betaSquare, pdg);

        if (energyLossBrems_ && noiseBrems_)
          this->noiseBrems(*noise, pSquare, betaSquare, pdg);
      } // end doNoise

    }

  } // end loop over steps

  if (momLoss >= mom) {
    Exception exc("MaterialEffects::effects ==> momLoss >= momentum, aborting extrapolation!",__LINE__,__FILE__);
    exc.setFatal();
    throw exc;
  }

  return momLoss;
}


void MaterialEffects::stepper(const RKTrackRep* rep,
                              M1x7& state7,
                              const Scalar& mom, // momentum
                              Scalar& relMomLoss, // relative momloss for the step will be added
                              const int& pdg,
                              MaterialProperties& currentMaterial,
                              StepLimits& limits,
                              bool varField)
{

  static const Scalar maxRelMomLoss = .01; // maximum relative momentum loss allowed
  static const Scalar Pmin   = 4.E-3;           // minimum momentum for propagation [GeV]
  static const Scalar minStep = 1.E-4; // 1 µm

  // check momentum
  if(mom < Pmin){
    std::ostringstream sstream;
    sstream << "MaterialEffects::stepper ==> momentum too low: " << mom*1000. << " MeV";
    Exception exc(sstream.str(),__LINE__,__FILE__);
    exc.setFatal();
    throw exc;
  }

  // Trivial cases
  if (noEffects_)
    return;

  if (materialInterface_ == nullptr) {
    std::string msg("MaterialEffects hasn't been initialized with a correct AbsMaterialInterface pointer!");
    std::runtime_error err(msg);
    throw err;
  }

  if (relMomLoss > maxRelMomLoss) {
    limits.setLimit(stp_momLoss, 0);
    return;
  }


  Scalar sMax = limits.getLowestLimitSignedVal(); // signed

  if (fabs(sMax) < minStep)
    return;

  // make minStep
  state7[0] += limits.getStepSign() * minStep * state7[3];
  state7[1] += limits.getStepSign() * minStep * state7[4];
  state7[2] += limits.getStepSign() * minStep * state7[5];

  materialInterface_->initTrack(state7[0], state7[1], state7[2],
                                limits.getStepSign() * state7[3], limits.getStepSign() * state7[4], limits.getStepSign() * state7[5]);

  materialInterface_->getMaterialParameters(matDensity_, matZ_, matA_, radiationLength_, mEE_);
  currentMaterial.setMaterialProperties(matDensity_, matZ_, matA_, radiationLength_, mEE_);


  if (debugLvl_ > 0) {
    debugOut << "     currentMaterial "; currentMaterial.Print();
  }

  // limit due to momloss
  Scalar relMomLossPer_cm(0);
  stepSize_ = 1.; // set stepsize for momLoss calculation

  if (matZ_ > 1.E-3) { // don't calculate energy loss for vacuum
    relMomLossPer_cm = this->momentumLoss(limits.getStepSign(), mom, true, pdg) / mom;
  }

  Scalar maxStepMomLoss = fabs((maxRelMomLoss - fabs(relMomLoss)) / relMomLossPer_cm); // >= 0
  limits.setLimit(stp_momLoss, maxStepMomLoss);

  if (debugLvl_ > 0) {
    debugOut << "     momLoss exceeded after a step of " <<  maxStepMomLoss
        << "; relMomLoss up to now = " << relMomLoss << "\n";
  }


  // now look for boundaries
  sMax = limits.getLowestLimitSignedVal();

  stepSize_ = limits.getStepSign() * minStep;
  MaterialProperties materialAfter;
  M1x3 SA;
  Scalar boundaryStep(sMax);

  for (unsigned int i=0; i<100; ++i) {
    if (debugLvl_ > 0) {
      debugOut << "     find next boundary\n";
    }
    Scalar step =  materialInterface_->findNextBoundary(rep, state7, boundaryStep, varField);

    if (debugLvl_ > 0) {
      if (step == 0) {
        debugOut << "     materialInterface_ returned a step of 0 \n";
      }
    }

    stepSize_ += step;
    boundaryStep -= step;

    if (debugLvl_ > 0) {
      debugOut << "     made a step of " << step << "\n";
    }

    if (! ignoreBoundariesBetweenEqualMaterials_)
      break;

    if (fabs(stepSize_) >= fabs(sMax))
      break;

    // propagate with found step to boundary
    rep->RKPropagate(state7, nullptr, SA, step, varField);

    // make minStep to cross boundary
    state7[0] += limits.getStepSign() * minStep * state7[3];
    state7[1] += limits.getStepSign() * minStep * state7[4];
    state7[2] += limits.getStepSign() * minStep * state7[5];

    materialInterface_->initTrack(state7[0], state7[1], state7[2],
                                  limits.getStepSign() * state7[3], limits.getStepSign() * state7[4], limits.getStepSign() * state7[5]);

    materialInterface_->getMaterialParameters(materialAfter);

    if (debugLvl_ > 0) {
      debugOut << "     material after step: "; materialAfter.Print();
    }

    if (materialAfter != currentMaterial)
      break;
  }

  limits.setLimit(stp_boundary, stepSize_);


  relMomLoss += relMomLossPer_cm * limits.getLowestLimitVal();
}



//---- Energy-loss and Noise calculations -----------------------------------------

Scalar MaterialEffects::momentumLoss(Scalar stepSign, Scalar mom, bool linear, const int pdg)
{
  const auto mass = getParticleMass(pdg);
  const auto charge = getParticleCharge(pdg);

  Scalar E0 = hypot(mom, mass);
  Scalar step = stepSize_*stepSign; // signed


  // calc dEdx_, also needed in noiseBetheBloch!
  // using fourth order Runge Kutta
  //k1 = f(t0,y0)
  //k2 = f(t0 + h/2, y0 + h/2 * k1)
  //k3 = f(t0 + h/2, y0 + h/2 * k2)
  //k4 = f(t0 + h,   y0 + h   * k3)

  // This means in our case:
  //dEdx1 = dEdx(x0,       E0)
  //dEdx2 = dEdx(x0 + h/2, E1); E1 = E0 + h/2 * dEdx1
  //dEdx3 = dEdx(x0 + h/2, E2); E2 = E0 + h/2 * dEdx2
  //dEdx4 = dEdx(x0 + h,   E3); E3 = E0 + h   * dEdx3

  Scalar dEdx1 = dEdx(E0, mass, charge, pdg); // dEdx(x0,p0)

  if (linear) {
    dEdx_ = dEdx1;
  }
  else { // RK4
    Scalar E1 = E0 - dEdx1*step/2.;
    Scalar dEdx2 = dEdx(E1, mass, charge, pdg); // dEdx(x0 + h/2, E0 + h/2 * dEdx1)

    Scalar E2 = E0 - dEdx2*step/2.;
    Scalar dEdx3 = dEdx(E2, mass, charge, pdg); // dEdx(x0 + h/2, E0 + h/2 * dEdx2)

    Scalar E3 = E0 - dEdx3*step;
    Scalar dEdx4 = dEdx(E3, mass, charge, pdg); // dEdx(x0 + h, E0 + h * dEdx3)

    dEdx_ = (dEdx1 + 2.*dEdx2 + 2.*dEdx3 + dEdx4)/6.;
  }

  E_ = E0 - dEdx_*step*0.5;

  Scalar dE = step*dEdx_; // positive for positive stepSign

  Scalar momLoss(0);

  if (E0 - dE <= mass) {
    // Step would stop particle (E_kin <= 0).
    return momLoss = mom;
  }
  else momLoss = mom - sqrt(pow(E0 - dE, 2) - mass*mass); // momLoss; positive for positive stepSign

  if (debugLvl_ > 0) {
    debugOut << "      MaterialEffects::momentumLoss: mom = " << mom << "; E0 = " << E0
        << "; dEdx = " << dEdx_
        << "; dE = " << dE << "; mass = " << mass << "\n";
  }

  //assert(momLoss * stepSign >= 0);

  return momLoss;
}


Scalar MaterialEffects::dEdx(const Scalar energy, const Scalar mass, const int charge, const int pdg) const {

  if (energy <= mass) {
    Exception exc("MaterialEffects::dEdx - Energy <= mass",__LINE__,__FILE__);
    exc.setFatal();
    throw exc;
  }

  const Scalar gamma = energy / mass;
  const Scalar gammaSquare = gamma * gamma;
  const Scalar betaSquare = 1 - 1 / gammaSquare;
  const Scalar mom = energy * sqrt(betaSquare);

  Scalar result(0);

  if (energyLossBetheBloch_)
    result += dEdxBetheBloch(betaSquare, gamma, gammaSquare, mass, charge);

  if (energyLossBrems_)
    result += dEdxBrems(mom, pdg);

  return result;
}


Scalar MaterialEffects::dEdxBetheBloch(const Scalar betaSquare, const Scalar gamma, const Scalar gammaSquare,
                                       const Scalar mass, const int charge) const
{
  static const Scalar betaGammaMin(0.05);

  if (betaSquare*gammaSquare < betaGammaMin*betaGammaMin) {
    Exception exc("MaterialEffects::dEdxBetheBloch ==> beta*gamma < 0.05, Bethe-Bloch implementation not valid anymore!",__LINE__,__FILE__);
    exc.setFatal();
    throw exc;
  }
  // calc dEdx_, also needed in noiseBetheBloch!
  Scalar result( 0.307075 * matZ_ / matA_ * matDensity_ / betaSquare * charge * charge);
  Scalar massRatio( me_ / mass );
  Scalar argument( gammaSquare * betaSquare * me_ * 1.E3 * 2. / ((1.E-6 * mEE_) *
      sqrt(1. + 2. * gamma * massRatio + massRatio * massRatio)) );
  result *= log(argument) - betaSquare; // Bethe-Bloch [MeV/cm]
  result *= 1.E-3;  // in GeV/cm, hence 1.e-3
  if (result < 0.) {
    result = 0;
  }

  return result;
}


void MaterialEffects::noiseBetheBloch(M7x7& noise, Scalar mom, Scalar betaSquare, Scalar gamma, Scalar gammaSquare, const int pdg) const
{
  const auto charge = getParticleCharge(pdg);
  const auto mass = getParticleMass(pdg);

  // Code ported from GEANT 3 (erland.F)

  // ENERGY LOSS FLUCTUATIONS; calculate sigma^2(E);
  Scalar sigma2E ( 0. );
  Scalar zeta  ( 153.4E3 * charge * charge / betaSquare * matZ_ / matA_ * matDensity_ * fabs(stepSize_) ); // eV
  Scalar Emax  ( 2.E9 * me_ * betaSquare * gammaSquare / (1. + 2.*gamma * me_ / mass + (me_ / mass) * (me_ / mass)) ); // eV
  Scalar kappa ( zeta / Emax );

  if (kappa > 0.01) { // Vavilov-Gaussian regime
    sigma2E += zeta * Emax * (1. - betaSquare / 2.); // eV^2
  } else { // Urban/Landau approximation
    // calculate number of collisions Nc
    Scalar I = 16. * pow(matZ_, 0.9); // eV
    Scalar f2 = 0.;
    if (matZ_ > 2.) f2 = 2. / matZ_;
    Scalar f1 = 1. - f2;
    Scalar e2 = 10.*matZ_ * matZ_; // eV
    Scalar e1 = pow((I / pow(e2, f2)), 1. / f1); // eV

    Scalar mbbgg2 = 2.E9 * mass * betaSquare * gammaSquare; // eV
    Scalar Sigma1 = dEdx_ * 1.0E9 * f1 / e1 * (log(mbbgg2 / e1) - betaSquare) / (log(mbbgg2 / I) - betaSquare) * 0.6; // 1/cm
    Scalar Sigma2 = dEdx_ * 1.0E9 * f2 / e2 * (log(mbbgg2 / e2) - betaSquare) / (log(mbbgg2 / I) - betaSquare) * 0.6; // 1/cm
    Scalar Sigma3 = dEdx_ * 1.0E9 * Emax / (I * (Emax + I) * log((Emax + I) / I)) * 0.4; // 1/cm

    Scalar Nc = (Sigma1 + Sigma2 + Sigma3) * fabs(stepSize_);

    if (Nc > 50.) { // truncated Landau distribution
      Scalar sigmaalpha = 15.76;
      // calculate sigmaalpha  (see GEANT3 manual W5013)
      Scalar RLAMED = -0.422784 - betaSquare - log(zeta / Emax);
      Scalar RLAMAX =  0.60715 + 1.1934 * RLAMED + (0.67794 + 0.052382 * RLAMED) * exp(0.94753 + 0.74442 * RLAMED);
      // from lambda max to sigmaalpha=sigma (empirical polynomial)
      if (RLAMAX <= 1010.) {
        sigmaalpha =  1.975560
                      + 9.898841e-02 * RLAMAX
                      - 2.828670e-04 * RLAMAX * RLAMAX
                      + 5.345406e-07 * pow(RLAMAX, 3.)
                      - 4.942035e-10 * pow(RLAMAX, 4.)
                      + 1.729807e-13 * pow(RLAMAX, 5.);
      } else { sigmaalpha = 1.871887E+01 + 1.296254E-02 * RLAMAX; }
      // alpha=54.6  corresponds to a 0.9996 maximum cut
      if (sigmaalpha > 54.6) sigmaalpha = 54.6;
      sigma2E += sigmaalpha * sigmaalpha * zeta * zeta; // eV^2
    } else { // Urban model
      static const Scalar alpha = 0.996;
      Scalar Ealpha  = I / (1. - (alpha * Emax / (Emax + I))); // eV
      Scalar meanE32 = I * (Emax + I) / Emax * (Ealpha - I); // eV^2
      sigma2E += fabs(stepSize_) * (Sigma1 * e1 * e1 + Sigma2 * e2 * e2 + Sigma3 * meanE32); // eV^2
    }
  }

  sigma2E *= 1.E-18; // eV -> GeV

  // update noise matrix, using linear error propagation from E to q/p
  noise[6 * 7 + 6] += charge*charge/betaSquare / pow(mom, 4) * sigma2E;
}


void MaterialEffects::noiseCoulomb(M7x7& noise,
                                   const M1x3& direction, Scalar momSquare, Scalar betaSquare, const int pdg) const
{
  const auto charge = getParticleCharge(pdg);
  // MULTIPLE SCATTERING; calculate sigma^2
  Scalar sigma2 = 0;
  assert(mscModelCode_ == 0 || mscModelCode_ == 1);
  const Scalar step = fabs(stepSize_);
  const Scalar step2 = step * step;
  if (mscModelCode_ == 0) {// PANDA report PV/01-07 eq(43); linear in step length
    sigma2 = 225.E-6 * charge * charge / (betaSquare * momSquare) * step / radiationLength_ * matZ_ / (matZ_ + 1) * log(159.*pow(matZ_, -1. / 3.)) / log(287.*pow(matZ_, -0.5)); // sigma^2 = 225E-6*z^2/mom^2 * XX0/beta_^2 * Z/(Z+1) * ln(159*Z^(-1/3))/ln(287*Z^(-1/2)

  } else if (mscModelCode_ == 1) { //Highland not linear in step length formula taken from PDG book 2011 edition
    Scalar stepOverRadLength = step / radiationLength_;
    Scalar logCor = (1 + 0.038 * log(stepOverRadLength));
    sigma2 = 0.0136 * 0.0136 * charge * charge / (betaSquare * momSquare) * stepOverRadLength * logCor * logCor;
  }
  //assert(sigma2 >= 0.0);
  sigma2 = (sigma2 > 0.0 ? sigma2 : 0.0);
  //XXX debugOut << "MaterialEffects::noiseCoulomb the MSC variance is " << sigma2 << std::endl;

  M7x7 noiseAfter; // will hold the new MSC noise to cause by the current stepSize_ length
  std::fill(noiseAfter.begin(), noiseAfter.end(), 0);

  const M1x3& a = direction; // as an abbreviation
  // This calculates the MSC angular spread in the 7D global
  // coordinate system.  See PDG 2010, Sec. 27.3 for formulae.
  noiseAfter[0 * 7 + 0] =  sigma2 * step2 / 3.0 * (1 - a[0]*a[0]);
  noiseAfter[1 * 7 + 0] = -sigma2 * step2 / 3.0 * a[0]*a[1];
  noiseAfter[2 * 7 + 0] = -sigma2 * step2 / 3.0 * a[0]*a[2];
  noiseAfter[3 * 7 + 0] =  sigma2 * step * 0.5 * (1 - a[0]*a[0]);
  noiseAfter[4 * 7 + 0] = -sigma2 * step * 0.5 * a[0]*a[1];
  noiseAfter[5 * 7 + 0] = -sigma2 * step * 0.5 * a[0]*a[1];
  noiseAfter[0 * 7 + 1] = noiseAfter[1 * 7 + 0];
  noiseAfter[1 * 7 + 1] =  sigma2 * step2 / 3.0 * (1 - a[1]*a[1]);
  noiseAfter[2 * 7 + 1] = -sigma2 * step2 / 3.0 * a[1]*a[2];
  noiseAfter[3 * 7 + 1] = noiseAfter[4 * 7 + 0]; // Cov(x,a_y) = Cov(y,a_x)
  noiseAfter[4 * 7 + 1] =  sigma2 * step * 0.5 * (1 - a[1] * a[1]);
  noiseAfter[5 * 7 + 1] = -sigma2 * step * 0.5 * a[1]*a[2];
  noiseAfter[0 * 7 + 2] = noiseAfter[2 * 7 + 0];
  noiseAfter[1 * 7 + 2] = noiseAfter[2 * 7 + 1];
  noiseAfter[2 * 7 + 2] =  sigma2 * step2 / 3.0 * (1 - a[2]*a[2]);
  noiseAfter[3 * 7 + 2] = noiseAfter[5 * 7 + 0]; // Cov(z,a_x) = Cov(x,a_z)
  noiseAfter[4 * 7 + 2] = noiseAfter[5 * 7 + 1]; // Cov(y,a_z) = Cov(z,a_y)
  noiseAfter[5 * 7 + 2] =  sigma2 * step * 0.5 * (1 - a[2]*a[2]);
  noiseAfter[0 * 7 + 3] = noiseAfter[3 * 7 + 0];
  noiseAfter[1 * 7 + 3] = noiseAfter[3 * 7 + 1];
  noiseAfter[2 * 7 + 3] = noiseAfter[3 * 7 + 2];
  noiseAfter[3 * 7 + 3] =  sigma2 * (1 - a[0]*a[0]);
  noiseAfter[4 * 7 + 3] = -sigma2 * a[0]*a[1];
  noiseAfter[5 * 7 + 3] = -sigma2 * a[0]*a[2];
  noiseAfter[0 * 7 + 4] = noiseAfter[4 * 7 + 0];
  noiseAfter[1 * 7 + 4] = noiseAfter[4 * 7 + 1];
  noiseAfter[2 * 7 + 4] = noiseAfter[4 * 7 + 2];
  noiseAfter[3 * 7 + 4] = noiseAfter[4 * 7 + 3];
  noiseAfter[4 * 7 + 4] =  sigma2 * (1 - a[1]*a[1]);
  noiseAfter[5 * 7 + 4] = -sigma2 * a[1]*a[2];
  noiseAfter[0 * 7 + 5] = noiseAfter[5 * 7 + 0];
  noiseAfter[1 * 7 + 5] = noiseAfter[5 * 7 + 1];
  noiseAfter[2 * 7 + 5] = noiseAfter[5 * 7 + 2];
  noiseAfter[3 * 7 + 5] = noiseAfter[5 * 7 + 3];
  noiseAfter[4 * 7 + 5] = noiseAfter[5 * 7 + 4];
  noiseAfter[5 * 7 + 5] = sigma2 * (1 - a[2]*a[2]);
//    debugOut << "new noise\n";
//    RKTools::printDim(noiseAfter, 7,7);
  for (unsigned int i = 0; i < 7 * 7; ++i) {
    noise[i] += noiseAfter[i];
  }
}


Scalar MaterialEffects::dEdxBrems(Scalar mom, const int pdg) const
{

  // Code ported from GEANT 3 (gbrele.F)

  if (abs(pdg) != 11) return 0; // only for electrons and positrons

#if !defined(BETHE)
  static const Scalar C[101] = { 0.0, -0.960613E-01, 0.631029E-01, -0.142819E-01, 0.150437E-02, -0.733286E-04, 0.131404E-05, 0.859343E-01, -0.529023E-01, 0.131899E-01, -0.159201E-02, 0.926958E-04, -0.208439E-05, -0.684096E+01, 0.370364E+01, -0.786752E+00, 0.822670E-01, -0.424710E-02, 0.867980E-04, -0.200856E+01, 0.129573E+01, -0.306533E+00, 0.343682E-01, -0.185931E-02, 0.392432E-04, 0.127538E+01, -0.515705E+00, 0.820644E-01, -0.641997E-02, 0.245913E-03, -0.365789E-05, 0.115792E+00, -0.463143E-01, 0.725442E-02, -0.556266E-03, 0.208049E-04, -0.300895E-06, -0.271082E-01, 0.173949E-01, -0.452531E-02, 0.569405E-03, -0.344856E-04, 0.803964E-06, 0.419855E-02, -0.277188E-02, 0.737658E-03, -0.939463E-04, 0.569748E-05, -0.131737E-06, -0.318752E-03, 0.215144E-03, -0.579787E-04, 0.737972E-05, -0.441485E-06, 0.994726E-08, 0.938233E-05, -0.651642E-05, 0.177303E-05, -0.224680E-06, 0.132080E-07, -0.288593E-09, -0.245667E-03, 0.833406E-04, -0.129217E-04, 0.915099E-06, -0.247179E-07, 0.147696E-03, -0.498793E-04, 0.402375E-05, 0.989281E-07, -0.133378E-07, -0.737702E-02, 0.333057E-02, -0.553141E-03, 0.402464E-04, -0.107977E-05, -0.641533E-02, 0.290113E-02, -0.477641E-03, 0.342008E-04, -0.900582E-06, 0.574303E-05, 0.908521E-04, -0.256900E-04, 0.239921E-05, -0.741271E-07, -0.341260E-04, 0.971711E-05, -0.172031E-06, -0.119455E-06, 0.704166E-08, 0.341740E-05, -0.775867E-06, -0.653231E-07, 0.225605E-07, -0.114860E-08, -0.119391E-06, 0.194885E-07, 0.588959E-08, -0.127589E-08, 0.608247E-10};
  static const Scalar xi = 2.51, beta = 0.99, vl = 0.00004;
#endif
#if defined(BETHE) // no MIGDAL corrections
  static const Scalar C[101] = { 0.0, 0.834459E-02, 0.443979E-02, -0.101420E-02, 0.963240E-04, -0.409769E-05, 0.642589E-07, 0.464473E-02, -0.290378E-02, 0.547457E-03, -0.426949E-04, 0.137760E-05, -0.131050E-07, -0.547866E-02, 0.156218E-02, -0.167352E-03, 0.101026E-04, -0.427518E-06, 0.949555E-08, -0.406862E-02, 0.208317E-02, -0.374766E-03, 0.317610E-04, -0.130533E-05, 0.211051E-07, 0.158941E-02, -0.385362E-03, 0.315564E-04, -0.734968E-06, -0.230387E-07, 0.971174E-09, 0.467219E-03, -0.154047E-03, 0.202400E-04, -0.132438E-05, 0.431474E-07, -0.559750E-09, -0.220958E-02, 0.100698E-02, -0.596464E-04, -0.124653E-04, 0.142999E-05, -0.394378E-07, 0.477447E-03, -0.184952E-03, -0.152614E-04, 0.848418E-05, -0.736136E-06, 0.190192E-07, -0.552930E-04, 0.209858E-04, 0.290001E-05, -0.133254E-05, 0.116971E-06, -0.309716E-08, 0.212117E-05, -0.103884E-05, -0.110912E-06, 0.655143E-07, -0.613013E-08, 0.169207E-09, 0.301125E-04, -0.461920E-04, 0.871485E-05, -0.622331E-06, 0.151800E-07, -0.478023E-04, 0.247530E-04, -0.381763E-05, 0.232819E-06, -0.494487E-08, -0.336230E-04, 0.223822E-04, -0.384583E-05, 0.252867E-06, -0.572599E-08, 0.105335E-04, -0.567074E-06, -0.216564E-06, 0.237268E-07, -0.658131E-09, 0.282025E-05, -0.671965E-06, 0.565858E-07, -0.193843E-08, 0.211839E-10, 0.157544E-04, -0.304104E-05, -0.624410E-06, 0.120124E-06, -0.457445E-08, -0.188222E-05, -0.407118E-06, 0.375106E-06, -0.466881E-07, 0.158312E-08, 0.945037E-07, 0.564718E-07, -0.319231E-07, 0.371926E-08, -0.123111E-09};
  static const Scalar xi = 2.10, beta = 1.00, vl = 0.001;
#endif

  Scalar BCUT = 10000.; // energy up to which soft bremsstrahlung energy loss is calculated

  static const Scalar THIGH = 100., CHIGH = 50.;
  Scalar dedxBrems = 0.;

  if (BCUT > 0.) {
    Scalar T, kc;

    if (BCUT > mom) BCUT = mom; // confine BCUT to mom_

    // T=mom_,  confined to THIGH
    // kc=BCUT, confined to CHIGH ??
    if (mom > THIGH) {
      T = THIGH;
      if (BCUT >= THIGH)
        kc = CHIGH;
      else
        kc = BCUT;
    } else {
      T = mom;
      kc = BCUT;
    }

    Scalar E = T + me_; // total electron energy
    if (BCUT > T)
      kc = T;

    Scalar X = log(T / me_);
    Scalar Y = log(kc / (E * vl));

    Scalar XX;
    int    K;
    Scalar S = 0., YY = 1.;

    for (unsigned int I = 1; I <= 2; ++I) {
      XX = 1.;
      for (unsigned int J = 1; J <= 6; ++J) {
        K = 6 * I + J - 6;
        S += C[K] * XX * YY;
        XX *= X;
      }
      YY *= Y;
    }

    for (unsigned int I = 3; I <= 6; ++I) {
      XX = 1.;
      for (unsigned int J = 1; J <= 6; ++J) {
        K = 6 * I + J - 6;
        if (Y > 0.)
          K += 24;
        S += C[K] * XX * YY;
        XX *= X;
      }
      YY *= Y;
    }

    Scalar SS = 0.;
    YY = 1.;

    for (unsigned int I = 1; I <= 2; ++I) {
      XX = 1.;
      for (unsigned int J = 1; J <= 5; ++J) {
        K = 5 * I + J + 55;
        SS += C[K] * XX * YY;
        XX *= X;
      }
      YY *= Y;
    }

    for (unsigned int I = 3; I <= 5; ++I) {
      XX = 1.;
      for (unsigned int J = 1; J <= 5; ++J) {
        K = 5 * I + J + 55;
        if (Y > 0.)
          K += 15;
        SS += C[K] * XX * YY;
        XX *= X;
      }
      YY *= Y;
    }

    S += matZ_ * SS;

    if (S > 0.) {
      Scalar CORR = 1.;
#if !defined(BETHE)
      CORR = 1. / (1. + 0.805485E-10 * matDensity_ * matZ_ * E * E / (matA_ * kc * kc)); // MIGDAL correction factor
#endif

      // We use exp(beta * log(...) here because pow(..., beta) is
      // REALLY slow and we don't need ultimate numerical precision
      // for this approximation.
      Scalar FAC = matZ_ * (matZ_ + xi) * E * E / (E + me_);
      if (beta == 1.)  // That is the #ifdef BETHE case
        FAC *= kc * CORR / T;
      else
        FAC *= exp(beta * log(kc * CORR / T));
      if (FAC <= 0.)
        return 0.;
      dedxBrems = FAC * S;


      if (mom >= THIGH) {
        Scalar RAT;
        if (BCUT < THIGH) {
          RAT = BCUT / mom;
          S = (1. - 0.5 * RAT + 2.*RAT * RAT / 9.);
          RAT = BCUT / T;
          S /= 1. - 0.5 * RAT + 2.*RAT * RAT / 9.;
        } else {
          RAT = BCUT / mom;
          S = BCUT * (1. - 0.5 * RAT + 2.*RAT * RAT / 9.);
          RAT = kc / T;
          S /= kc * (1. - 0.5 * RAT + 2.*RAT * RAT / 9.);
        }
        dedxBrems *= S; // GeV barn
      }

      dedxBrems *= 0.60221367 * matDensity_ / matA_; // energy loss dE/dx [GeV/cm]
    }
  }

  if (dedxBrems < 0.)
    dedxBrems = 0;

  Scalar factor = 1.; // positron correction factor

  if (pdg == -11) {
    static const Scalar AA = 7522100., A1 = 0.415, A3 = 0.0021, A5 = 0.00054;

    Scalar ETA = 0.;
    if (matZ_ > 0.) {
      Scalar X = log(AA * mom / (matZ_ * matZ_));
      if (X > -8.) {
        if (X >= +9.) ETA = 1.;
        else {
          Scalar W = A1 * X + A3 * pow(X, 3.) + A5 * pow(X, 5.);
          ETA = 0.5 + atan(W) / M_PI;
        }
      }
    }

    if (ETA < 0.0001)
      factor = 1.E-10;
    else if (ETA > 0.9999)
      factor = 1.;
    else {
      Scalar E0 = BCUT / mom;
      if (E0 > 1.)
        E0 = 1.;
      if (E0 < 1.E-8)
        factor = 1.;
      else
        factor = ETA * (1. - pow(1. - E0, 1. / ETA)) / E0;
    }
  }

  return factor * dedxBrems; //always positive
}


void MaterialEffects::noiseBrems(M7x7& noise, Scalar momSquare, Scalar betaSquare, const int pdg) const
{
  // Code ported from GEANT 3 (erland.F) and simplified
  // E \approx p is assumed.
  // the factor  1.44 is not in the original Bethe-Heitler model.
  // It seems to be some empirical correction copied over from some other project.

  if (abs(pdg) != 11) return; // only for electrons and positrons

  const auto charge = getParticleCharge(pdg);
  Scalar minusXOverLn2  = -1.442695 * fabs(stepSize_) / radiationLength_;
  Scalar sigma2E = 1.44*(pow(3., minusXOverLn2) - pow(4., minusXOverLn2)) * momSquare;
  sigma2E = std::max(sigma2E, 0.0); // must be positive
  
  // update noise matrix, using linear error propagation from E to q/p
  noise[6 * 7 + 6] += charge*charge/betaSquare / pow(momSquare, 2) * sigma2E;
}


void MaterialEffects::setDebugLvl(unsigned int lvl) {
  debugLvl_ = lvl;
  if (materialInterface_ and debugLvl_ > 1)
    materialInterface_->setDebugLvl(debugLvl_-1);
}


void MaterialEffects::drawdEdx(int pdg) {
  const auto mass = getParticleMass(pdg);
  const auto charge = getParticleCharge(pdg);

  stepSize_ = 1;

  materialInterface_->initTrack(0, 0, 0, 1, 1, 1);
  materialInterface_->getMaterialParameters(matDensity_, matZ_, matA_, radiationLength_, mEE_);


  Scalar minMom = 0.00001;
  Scalar maxMom = 10000;
  int nSteps(10000);
  Scalar logStepSize = (log10(maxMom) - log10(minMom)) / (nSteps-1);

  TH1D hdEdxBethe("dEdxBethe", "dEdxBethe; log10(mom)", nSteps, log10(minMom), log10(maxMom));
  TH1D hdEdxBrems("dEdxBrems", "dEdxBrems; log10(mom)", nSteps, log10(minMom), log10(maxMom));

  for (int i=0; i<nSteps; ++i) {
    Scalar mom = pow(10., log10(minMom) + i*logStepSize);
    Scalar E = hypot(mom, mass);

    energyLossBrems_ = false;
    energyLossBetheBloch_ = true;

    try {
      hdEdxBethe.Fill(log10(mom), dEdx(E, mass, charge, pdg));
    }
    catch (...) {

    }


    //debugOut<< "E = " << E << "; dEdx = " << dEdx(E) <<"\n";

    energyLossBrems_ = true;
    energyLossBetheBloch_ = false;
    try {
      hdEdxBrems.Fill(log10(mom), dEdx(E, mass, charge, pdg));
    }
    catch (...) {

    }
  }

  energyLossBrems_ = true;
  energyLossBetheBloch_ = true;

  std::string Result;//string which will contain the result
  std::stringstream convert; // stringstream used for the conversion
  convert << pdg;//add the value of Number to the characters in the stream
  Result = convert.str();//set Result to the content of the stream

  TFile outfile("dEdx_" + TString(Result) + ".root", "recreate");
  outfile.cd();
  hdEdxBethe.Write();
  hdEdxBrems.Write();
  outfile.Close();
}

} /* End of namespace genfit */


