/*!
 * \author Jeremias Berg - jeremiasberg@hmail.com
 *
 * @section LICENSE
 *   Loandra, Copyright (c) 2018, Jeremias Berg, Emir Demirovic, Peter Stuckey
 *  Open-WBO, Copyright (c) 2013-2017, Ruben Martins, Vasco Manquinho, Ines Lynce
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "Alg_PMRES.h"

using namespace openwbo;

/************************************************************************************************
 //
 // Rebuild MaxSAT solver
 //
 ************************************************************************************************/

/*_________________________________________________________________________________________________
  |
  |  rebuildWeightSolver : (strategy : int)  ->  [Solver *]
  |
  |   Description:
  |
  |    Rebuilds a SAT solver with the current MaxSAT formula using a
  |    weight-based strategy.
  |    Only soft clauses with weight greater or equal to 'currentWeight' are
  |    considered in the working MaxSAT formula.
  |
  |   For further details see:
  |     * Ruben Martins, Vasco Manquinho, Inês Lynce: On Partitioning for
  |       Maximum Satisfiability. ECAI 2012: 913-914
  |	    * Carlos Ansótegui, Maria Luisa Bonet, Joel Gabàs, Jordi Levy:
  |       Improving SAT-Based Weighted MaxSAT Solvers. CP 2012: 86-101
  |
  |   Pre-conditions:
  |     * Assumes that 'currentWeight' has been previously updated.
  |     * Assumes that the weight strategy is either '_WEIGHT_NORMAL_' or
  |       '_WEIGHT_DIVERSIFY_'.
  |
  |   Post-conditions:
  |    
  |
  |________________________________________________________________________________________________@*/
Solver *PMRES::updateSolver() {

  reserveSATVariables(solver, maxsat_formula->nVars());

  for (int i = vars_added; i < maxsat_formula->nVars(); i++)
    newSATVariable(solver);

  vars_added = maxsat_formula->nVars();

  for (int i = clauses_added; i < maxsat_formula->nHard(); i++)
    solver->addClause(maxsat_formula->getHardClause(i).clause);
  
  clauses_added = maxsat_formula->nHard();

  softs_added = maxsat_formula->nSoft();

  //We do not support these
  assert(maxsat_formula->nPB() == 0); 
  //We do not support these
  assert(maxsat_formula->nCard() == 0);
  return solver;
}

/*_________________________________________________________________________________________________
  |
  |  updateCurrentWeight : (strategy : int)  ->  [void]
  |
  |  Description:
  |
  |    Updates the value of 'currentWeight' with a predefined strategy.
  |
  |  Pre-conditions:
  |    * Assumes that the weight strategy is either '_WEIGHT_NORMAL_' or
  |      '_WEIGHT_DIVERSIFY_'.
  |
  |  Post-conditions:
  |    * 'currentWeight' is updated by this method.
  |
  |________________________________________________________________________________________________@*/
void PMRES::updateCurrentWeight(int strategy) {

  assert(strategy == _WEIGHT_NORMAL_ || strategy == _WEIGHT_DIVERSIFY_);
  if (!varyingresCG) {
    if (strategy == _WEIGHT_NORMAL_)
      maxsat_formula->setMaximumWeight(
          findNextWeight(maxsat_formula->getMaximumWeight()));
    else if (strategy == _WEIGHT_DIVERSIFY_)
      maxsat_formula->setMaximumWeight(
          findNextWeightDiversity(maxsat_formula->getMaximumWeight()));
  }
  else {
      updateDivisionFactor();
  }
  logPrint("CG New weight: " + std::to_string(maxsat_formula->getMaximumWeight()) + " at " + print_timeSinceStart());
}

/*_________________________________________________________________________________________________
  |
  |  findNextWeight : (weight : uint64_t)  ->  [uint64_t]
  |
  |  Description:
  |
  |    Finds the greatest weight that is smaller than the 'currentWeight'.
  |
  |  For further details see:
  |    * Ruben Martins, Vasco Manquinho, Inês Lynce: On Partitioning for Maximum
  |      Satisfiability. ECAI 2012: 913-914
  |
  |________________________________________________________________________________________________@*/
uint64_t PMRES::findNextWeight(uint64_t weight) {

  uint64_t nextWeight = 1;
  for (int i = 0; i < maxsat_formula->nSoft(); i++) {
    if (maxsat_formula->getSoftClause(i).weight > nextWeight &&
        maxsat_formula->getSoftClause(i).weight < weight)
      nextWeight = maxsat_formula->getSoftClause(i).weight;
  }

  return nextWeight;
}

/*_________________________________________________________________________________________________
  |
  |  findNextWeightDiversity : (weight : uint64_t)  ->  [uint64_t]
  |
  |  Description:
  |
  |  Finds the greatest weight that is smaller than the 'currentWeight' and
  |  respects a given ratio
  |  between the number of different weights and the number of soft clauses.
  |
  |  Pre-conditions:
  |    * Assumes that the weight strategy is '_WEIGHT_DIVERSIFY_'.
  |    * Assumes that 'unsatSearch' was call before (this implies that
  |      nbSatisfable > 0).
  |
  |  For further details see:
  |    * Carlos Ansótegui, Maria Luisa Bonet, Joel Gabàs, Jordi Levy: Improving
  |      SAT-Based Weighted MaxSAT Solvers. CP 2012: 86-101
  |
  |________________________________________________________________________________________________@*/
uint64_t PMRES::findNextWeightDiversity(uint64_t weight) {

  assert(weightStrategy == _WEIGHT_DIVERSIFY_);
  assert(nbSatisfiable > 0); // Assumes that unsatSearch was done before.

  uint64_t nextWeight = weight;
  int nbClauses = 0;
  std::set<uint64_t> nbWeights;
  float alpha = 1.25;

  bool findNext = false;

  for (;;) {
    if (nbSatisfiable > 1 || findNext)
      nextWeight = findNextWeight(nextWeight);

    nbClauses = 0;
    nbWeights.clear();
    for (int i = 0; i < maxsat_formula->nSoft(); i++) {
      if (maxsat_formula->getSoftClause(i).weight >= nextWeight) {
        nbClauses++;
        nbWeights.insert(maxsat_formula->getSoftClause(i).weight);
      }
    }

    if ( ( (float)nbClauses / nbWeights.size() > alpha && nbClauses > nbCurrentSoft) ||
        nbClauses == nRealSoft())
      break;

    if (nbSatisfiable == 1 && !findNext)
      findNext = true;
  }

  return nextWeight;
}
/*_________________________________________________________________________________________________
  |
  |  hardenclauses : void
  |
  |  Description:
  |
  |  Hardens soft clauses that are heavier than the current known gap between the upper and lower bound
  |   
  |  Pre-conditions:
  |    
  |
  |  For further details see:
  |     Antonio Morgado, Federico Heras, and Joao Marques-Silva. 2012. Improvements to core-guided binary search for MaxSAT. 
  |     In Proceedings of the 15th international conference on Theory and Applications of Satisfiability Testing (SAT'12), 
  |     Alessandro Cimatti and Roberto Sebastiani (Eds.). Springer-Verlag, Berlin, Heidelberg, 284-297. 
  |
  |
  |________________________________________________________________________________________________@*/

  Solver *PMRES::hardenClauses() {    
	   uint64_t bound = ubCost - lbCost;
     logPrint("Hardening with gap: " + std::to_string(bound));
     int num_hardened_round = 0;
	   vec<lbool> &currentModel = solver->model;
	   maxw_nothardened = 0;
	   for (int i = 0; i < softs_added; i++)
		  {
			bool satisfied = false;
			if (maxsat_formula->getSoftClause(i).weight == bound) {
				 assert(maxsat_formula->getSoftClause(i).clause.size() == 1 );

          Lit l =  maxsat_formula->getSoftClause(i).clause[0];
          assert(var(l) < currentModel.size());

          if ((sign(l) && currentModel[var(l)] == l_False) ||
            (!sign(l) && currentModel[var(l)] == l_True)) {
            satisfied = true;
          }
			  }
			if (maxsat_formula->getSoftClause(i).weight > bound  || (maxsat_formula->getSoftClause(i).weight == bound && satisfied) ) {  // 
				vec<Lit> clause;
				clause.clear();
				 
				Lit l = maxsat_formula->getSoftClause(i).assumption_var;
				assert(l != lit_Undef);
				clause.push(~l);
				solver->addClause(clause);

        if (!hardenLazily()) 
          maxsat_formula->addHardClause(clause); 


				maxsat_formula->getSoftClause(i).weight = 0;
        maxsat_formula->getSoftClause(i).assumption_var = lit_Undef;
				num_hardened++;
				num_hardened_round++;
			}
			else if (maxsat_formula->getSoftClause(i).weight > maxw_nothardened) {
				maxw_nothardened = maxsat_formula->getSoftClause(i).weight;
			} 

			
		}
		logPrint("Hardened in total: " + std::to_string(num_hardened_round) + " clauses");
    logPrint("Hardening again at gap " + std::to_string(maxw_nothardened));
		return solver;
   }

   bool PMRES::hardenLazily() {
    return !delete_before_lin && !varyingres;
   }
   

/************************************************************************************************
 //
 // Varying resolution 
 //
 ************************************************************************************************/
  bool PMRES::enoughSoftAboveWeight(uint64_t weightCand) {
    assert(nbSatisfiable > 0); // Assumes that unsatSearch was done before.
    int nbClauses = 0;
    
    std::set<uint64_t> nbWeights;
    nbWeights.clear();
    
    float alpha = 1.25;

    for (int i = 0; i < maxsat_formula->nSoft(); i++) {
      if (maxsat_formula->getSoftClause(i).weight >= weightCand) {
        nbClauses++;
        nbWeights.insert(maxsat_formula->getSoftClause(i).weight);
      }
    }

    return (float)nbClauses / nbWeights.size() > alpha || nbClauses == nRealSoft();
  }
  
  
  void PMRES::resetMaximumWeight() {
    uint64_t maxW = 1;
    for (int i = 0; i < maxsat_formula->nSoft(); i++) {
      if (maxsat_formula->getSoftClause(i).weight > maxW) {
        maxW = maxsat_formula->getSoftClause(i).weight;
      }
    }
    maxsat_formula->setMaximumWeight(maxW);
  }

  void PMRES::updateDivisionFactor() {
    uint64_t nextFactor = maxsat_formula->getMaximumWeight() / varresFactor; //TODO parametrize
    while (!enoughSoftAboveWeight(nextFactor))
      nextFactor /= varresFactor; 
    maxsat_formula->setMaximumWeight(nextFactor);
    logPrint("CG Division Factor " + std::to_string(nextFactor));
  }

  void PMRES::updateDivisionFactorLinear() {
    uint64_t nextFactor = maxsat_formula->getMaximumWeight() / varresFactor;
    while (moreThanWeight(nextFactor) == nbCurrentSoft && nextFactor > 1 ) {
      nextFactor /= varresFactor; 
    }
    maxsat_formula->setMaximumWeight(nextFactor);
    logPrint("LIN New factor " + std::to_string(nextFactor));
  }

  int PMRES::moreThanWeight(uint64_t weightCand) {
    int test = 0;
    for (int i = 0; i < maxsat_formula->nSoft(); i++) {
      if (maxsat_formula->getSoftClause(i).weight >= weightCand) {
        test++;
      }
    }
    return test;

  }

  //Assumes formula->getMaxWeight() is equal to the maximum weight of the current soft clauses 
  void PMRES::initializeDivisionFactor(bool var) {
      if (!var) {
        maxsat_formula->setMaximumWeight(1);
        logPrint("Not doing varrres");
        logPrint("CG New factor " + std::to_string(1));
        return;
      }

      resetMaximumWeight();

      uint64_t maxW =  maxsat_formula->getMaximumWeight();
      int counter = 0; //64 bits, isokay  
      while (maxW > 0) {
          counter++;
          maxW = maxW / varresFactor; 
      }
      uint64_t weightCand = pow(varresFactor, counter - 1);
      while (!enoughSoftAboveWeight(weightCand))
        weightCand /= varresFactor; 

      logPrint("CG New factor " + std::to_string(weightCand));
      maxsat_formula->setMaximumWeight(weightCand);
  }



/************************************************************************************************
 //
 // Utils for core management
 //
 ************************************************************************************************/

/*_________________________________________________________________________________________________
  |
  |  PMRES : (lits : vec<Lit>&)  ->  [void]
  |
  |  Description:
  |
  |    The PMRES transformation
  |
  |  For further details see:
  |    * Nina Narodytska and Fahiem Bacchus. 2014. Maximum satisfiability using core-guided MAXSAT resolution. 
  |      In Proceedings of the Twenty-Eighth AAAI Conference on Artificial Intelligence (AAAI'14). AAAI Press 2717-2723.
  |
  |  Pre-conditions:
  |    * Assumes that 'core' is not empty.
  |
  |  Post-conditions:
  |    * 'hardClauses' are updated with the clauses that encode the PMRES
  |      constraint. Soft clauses are immediately added as hard and onyl the assumption is used in soft
  |
  |________________________________________________________________________________________________@*/
void PMRES::encodeMaxRes(vec<Lit> &core, uint64_t weightCore)
{

	assert(core.size() != 0); 

  int n = core.size(); 
  vec<Lit> dVars;
	vec<Lit> clause; // for adding stuff

	
  for (int i = 0; i < n - 1; i++)
    {
      Lit p = maxsat_formula->newLiteral();
      dVars.push(p);
    }
    // NEW HARD CLAUSES
  
  if (lins == 0) {
    clause.clear();
    core.copyTo(clause);
    maxsat_formula->addHardClause(clause);
  }

  if (n > 2) {
		for (int i = 0; i < n-2; i++) {
			// d_i -> (b_{i+1} v d_{i+1})
			// clause = { ~dVars[i], dVars[i + 1], core[i + 1] };
      // Not needed for completeness 
			
      if (lins == 0) {
      	clause.clear();
				clause.push(~dVars[i]);
				clause.push(dVars[i + 1]);
				clause.push(core[i + 1]);
				maxsat_formula->addHardClause(clause);
      }
		
		
			// (b_{i+1} v d_{i+1}) -> d_i
			// d_i v -b_{i+1}
			// clause = { dVars[i], ~core[i + 1] };
			clause.clear();
			clause.push(dVars[i]);
			clause.push(~core[i + 1]);
			maxsat_formula->addHardClause(clause);
			
			 // d_i v -d_{i+1}
			 // clause = { dVars[i], ~dVars[i + 1] };
			clause.clear();
			clause.push(dVars[i]);
			clause.push(~dVars[i + 1]);
			maxsat_formula->addHardClause(clause);	
		}
	}
    
    if (n > 1) {
		 // handle i = p - 1 case
		 // clause = { dVars[p - 2], ~core[p - 1] };
		 clause.clear();
		 clause.push(dVars[n - 2]);
		 clause.push(~core[n - 1]);
		 maxsat_formula->addHardClause(clause);
		 
		 // clause = { ~dVars[p - 2], core[p - 1] };
		 clause.clear();
		 clause.push(~dVars[n - 2]);
		 clause.push(core[n - 1]);
		 maxsat_formula->addHardClause(clause);
	}
	
	// NEW SOFT CLAUSES
    for (int i = 0; i < n-1; i++) {
		//clause = { ~b_i, ~d_i };
		clause.clear();
		clause.push(~core[i]);
		clause.push(~dVars[i]);
		addSoftClauseAndAssumptionVar(weightCore, clause);
	}

  
}

/*_________________________________________________________________________________________________
  |
  |  relaxCore : (conflict : vec<Lit>&) (weightCore : int) 
  |              ->  [void]
  |
  |  Description:
  |
  |    Relaxes the core as described in the original WBO paper.
  |
  |  For further details see:
  |    * Vasco Manquinho, Joao Marques-Silva, Jordi Planes: Algorithms for
  |      Weighted Boolean Optimization. SAT 2009: 495-508
  |
  |  Pre-conditions:
  |    * Assumes that the core ('conflict') is not empty.
  |    * Assumes that the weight of the core is not 0 (should always be greater
  |      than or equal to 1).
  |
  |  Post-conditions:
  |    * If the weight of the soft clause is the same as the weight of the core:
  |      - 'softClauses[indexSoft].relaxationVars' is updated with a new
  |        relaxation variable.
  |    * If the weight of the soft clause is not the same as the weight of the
  |      core:
  |      - 'softClauses[indexSoft].weight' is decreased by the weight of the
  |        core.
  |      - A new soft clause is created. This soft clause has the weight of the
  |        core.
  |      - A new assumption literal is created and attached to the new soft
  |        clause.
  |      - 'coreMapping' is updated to map the new soft clause to its assumption
  |        literal.
  |    * 'sumSizeCores' is updated.
  |
  |________________________________________________________________________________________________@*/
void PMRES::relaxCore(vec<Lit> &conflict, uint64_t weightCore) {

  assert(conflict.size() > 0);
  assert(weightCore > 0);


  for (int i = 0; i < conflict.size(); i++) {
    int indexSoft = coreMapping[conflict[i]];
    assert(maxsat_formula->getSoftClause(indexSoft).weight >= weightCore);
    maxsat_formula->getSoftClause(indexSoft).weight -= weightCore;

    if(maxsat_formula->getSoftClause(indexSoft).weight == 0) {
      maxsat_formula->getSoftClause(indexSoft).assumption_var = lit_Undef;
      num_hardened++;
    }
  }
  encodeMaxRes(conflict, weightCore);
  sumSizeCores += conflict.size();
}

/*_________________________________________________________________________________________________
  |
  |  computeCostCore : (conflict : vec<Lit>&)  ->  [int]
  |
  |    Description:
  |
  |      Computes the cost of the core. The cost of a core is the minimum weight
  |      of the soft clauses that appear in that core.
  |
  |    Pre-conditions:
  |      * Assumes that 'conflict' is not empty.
  |
  |________________________________________________________________________________________________@*/
uint64_t PMRES::computeCostCore(const vec<Lit> &conflict) {

  assert(conflict.size() != 0);

  if (maxsat_formula->getProblemType() == _UNWEIGHTED_) {
    return 1;
  }

  uint64_t coreCost = UINT64_MAX;
  for (int i = 0; i < conflict.size(); i++) {
    int indexSoft = coreMapping[conflict[i]];
    if (maxsat_formula->getSoftClause(indexSoft).weight < coreCost)
      coreCost = maxsat_formula->getSoftClause(indexSoft).weight;
  }

  return coreCost;
}

/************************************************************************************************
 //
 // SEARCHES
 //
 ************************************************************************************************/

/*_________________________________________________________________________________________________
  |
  |  unsatSearch : [void] ->  [void]
  |
  |  Description:
  |
  |    Calls a SAT solver only on the hard clauses of the MaxSAT formula.
  |    If the hard clauses are unsatisfiable then the MaxSAT solver terminates
  |    and returns 'UNSATISFIABLE'.
  |    Otherwise, a model has been found and it is stored. Without this call,
  |    the termination of the MaxSAT solver is not guaranteed.
  |
  |  For further details see:
  |    * Carlos Ansótegui, Maria Luisa Bonet, Jordi Levy: SAT-based MaxSAT
  |      algorithms. Artif. Intell. 196: 77-105 (2013)
  |
  |  Post-conditions:
  |   * If the hard clauses are satisfiable then 'ubCost' is updated to the cost
  |     of the model.
  |   * If the working formula is satisfiable, then 'nbSatisfiable' is increased
  |     by 1. Otherwise, 'nbCores' is increased by 1.
  |
  |________________________________________________________________________________________________@*/
StatusCode PMRES::unsatSearch() {

  assert(assumptions.size() == 0);


  solver = updateSolver();

  softsSatisfied();
  lbool res = searchSATSolver(solver, assumptions);
  solver->resetFixes();
  
  if (res == l_False) {
    nbCores++;
    printAnswer(_UNSATISFIABLE_);
    return _UNSATISFIABLE_;
  } else if (res == l_True) {
    nbSatisfiable++;
    uint64_t beforecheck = ubCost;
    checkModel();
    uint64_t aftercheck = ubCost;
    assert(beforecheck >= aftercheck);    
  }
  return _SATISFIABLE_;
}
/*_________________________________________________________________________________________________
  |
  |  weightDisjointCores : [void] ->  [void]
  |
  |  Description:
  |
  |    Runs the sat solver repeadetly on the current settings, extracting and relaxing cores
  |    does not rebuild the SAT-solver, alter strat weight or harden clauses. 
  |
  |  For further details see:
  |    * Berg, J., & Järvisalo, M. (2017). Weight-Aware Core Extraction in SAT-Based MaxSAT Solving. CP.
  |
  |  Pre-conditions:
  |    * Assumes the setup method has been called
  |
  |  Post-conditions:
  |     * The hard clauses in formula reflect the found and relaxed cores. 
  |     * LB is updated.
  |________________________________________________________________________________________________@*/

  StatusCode PMRES::weightDisjointCores() {
    
    for (;;) {
      if(timeLimitCores > 0 && (time_t)timeLimitCores- timeSinceStart() <= 0 ) {
        return _UNKNOWN_;
      }
      if(timeLimitCores > 0) {
        logPrint("Core budget remaining " + std::to_string((time_t)timeLimitCores - timeSinceStart()));
        solver->setTimeBudget(timeLimitCores- timeSinceStart());
      }
      setAssumptions(assumptions);
      lbool res; 
      res = searchSATSolver(solver, assumptions);

      if (res == l_Undef) {
        //Interrupted
        return _UNKNOWN_;
      }

      if (res == l_False) {
      
        nbCores++;
        assert(solver->conflict.size() > 0);
        uint64_t coreCost = computeCostCore(solver->conflict);
        lbCost += coreCost;
        checkGap();
        if (verbosity > 0) {
         printf("c LB : %-12" PRIu64 " CS : %-12d W  : %-12" PRIu64 "\n", lbCost,
                solver->conflict.size(), coreCost);
        }
        relaxCore(solver->conflict, coreCost);
      }

      if (res == l_True) {
        return _SATISFIABLE_; 
      }
      if (lbCost > ubCost) {
        return _ERROR_;
      }

    }
  }

/*_________________________________________________________________________________________________
  |
  |  setup : [void] ->  [void]
  |
  |  Description:
  |
  |    Makes SAT solver and checks that solutions exist. Most other search methods assume this has been run-  
  |    RETURNS unsat if no solutions exist
  |
  |  Post-conditions:
  |     * SAT solver exists 
  |     * Solutions exists
  |     * 1 model exists
  |________________________________________________________________________________________________@*/
  StatusCode PMRES::setup() {
      initAssumptions();  
      solver = newSATSolver();
      solver->setSolutionBasedPhaseSaving(false);
      StatusCode rs = unsatSearch();
      
      

      if(varyingresCG) {
        initializeDivisionFactor(varyingresCG);
      }
      else {
        updateCurrentWeight(weightStrategy);
      }

      return rs;
  }




/*_________________________________________________________________________________________________
  |
  |  weightSearch : [void] ->  [void]
  |
  |  Description:
  |
  |    MaxSAT weight-based search. Considers the weights of soft clauses to find
  |    cores with larger weights first.
  |
  |  For further details see:
  |    * Ruben Martins, Vasco Manquinho, Inês Lynce: On Partitioning for Maximum
  |      Satisfiability. ECAI 2012: 913-914
  |    * Carlos Ansótegui, Maria Luisa Bonet, Joel Gabàs, Jordi Levy: Improving
  |      SAT-Based Weighted MaxSAT Solvers. CP 2012: 86-101
  |
  |  Pre-conditions:
  |    * Assumes 'weightStrategy' to be '_WEIGHT_NORMAL_' or
  |      '_WEIGHT_DIVERSIFY_'.
  |
  |  Post-conditions:
  |    * 'lbCost' is updated.
  |    * 'ubCost' is updated.
  |    * 'nbSatisfiable' is updated.
  |    * 'nbCores' is updated.
  |________________________________________________________________________________________________@*/
StatusCode PMRES::weightSearch() {

  assert(weightStrategy == _WEIGHT_NORMAL_ ||
         weightStrategy == _WEIGHT_DIVERSIFY_);
  inLinSearch = false;
  assert(timeLimitCores < 0);
  
  for (;;) {
    StatusCode us = weightDisjointCores(); 

    //LB phase proves optimality, current model is not for the current formula. 
    if (us == _OPTIMUM_) {
        printf("c LB = UB\n");
        return getModelAfterCG();
    }

    //At this point solver returned true and as such has a model
   
    nbSatisfiable++;
    uint64_t modelCost = computeCostModelPMRES(solver->model);
    if (modelCost < ubCost) {
        ubCost = modelCost;
        saveModel(solver->model);
        printBound(ubCost);
    }
    if (lbCost == ubCost) {
      if (verbosity > 0)
        printf("c LB = UB\n");
        printAnswer(_OPTIMUM_);
        return _OPTIMUM_;
    }
    if (nbCurrentSoft == nRealSoft()) {
      assert(modelCost == lbCost);
      if (lbCost < ubCost) {
        ubCost = lbCost;
        saveModel(solver->model);
        printBound(lbCost);
      }
        printAnswer(_OPTIMUM_);
        return _OPTIMUM_;
    } 
    if (ubCost - lbCost < maxw_nothardened) {
      solver = hardenClauses();
    }
    if (shouldUpdate()) {
      solver = updateSolver();
    } 
    else {
      updateCurrentWeight(weightStrategy);
    }
    
  }
}



/*_________________________________________________________________________________________________
  |
  |  weightPMRES+Linear: [void] ->  [void]
  |
  |  Description:
  |
  |    New idea, run weight aware disjoint phase and then do the rest by Linear search 
  |
  | 
  |________________________________________________________________________________________________@*/
StatusCode PMRES::coreGuidedLinearSearch() {
  inLinSearch = false;
  for (;;) {
    StatusCode us = weightDisjointCores(); 
    if (us == _OPTIMUM_) {
        printf("c LB = UB\n");
        return getModelAfterCG();
    }

    if (us == _UNKNOWN_ ) {
        logPrint("Interrupted core guided phase");
        if(shouldUpdate()) {
          logPrint("Updating solver at " + print_timeSinceStart());
          solver = updateSolver();
        }
        return linearSearch();
    }
    

    //At this point solver returned true and as such has a model
    assert(us == _SATISFIABLE_ );

    logPrint("SAT-During core guided phase at " + print_timeSinceStart());
    nbSatisfiable++;
    checkModel();

    if (lbCost == ubCost) {
      if (verbosity > 0)
        printf("c LB = UB\n");
        printAnswer(_OPTIMUM_);
        return _OPTIMUM_;
    }

   
     if (nbCurrentSoft == nRealSoft()) {
      uint64_t modelCost = computeCostModelPMRES(solver->model);
      assert(modelCost == lbCost);
      if (lbCost < ubCost) {
        ubCost = lbCost;
        saveModel(solver->model);
        printBound(lbCost);
      }
        printAnswer(_OPTIMUM_);
        return _OPTIMUM_;
    } 


   //if code gets here algorithm cant terminate yet   
   if (ubCost - lbCost < maxw_nothardened) {
        solver = hardenClauses();
      }

   if(relaxBeforeStrat) {
      logPrint("Relax 2 Strat");
      if(shouldUpdate()) {
          logPrint("Updating solver at " + print_timeSinceStart());
          solver = updateSolver();
      }
      else if (maxsat_formula->getMaximumWeight() > 1) {
              logPrint("Weight update at " + print_timeSinceStart());
              updateCurrentWeight(weightStrategy); 
              if (maxsat_formula->getMaximumWeight() == 1) {
                logPrint("Weight = 1 -> Done with cores at " + print_timeSinceStart());
                return linearSearch();
              }
      }
      else {
        return _ERROR_; // Should not get here
      }
      
   }
  else {
      logPrint("Strat 2 Relax");
      if (maxsat_formula->getMaximumWeight() > 1) {
              logPrint("Weight update at " + print_timeSinceStart());
              updateCurrentWeight(weightStrategy); 
                      
      }
      if (maxsat_formula->getMaximumWeight() == 1) {
        if(shouldUpdate()) {
          logPrint("Updating solver at " + print_timeSinceStart());
          solver = updateSolver();
        }
        return linearSearch();
        
      }
    }
  }
  //Code never gets here 
  return _ERROR_;
}

/*
  only if the cg phase proves optimality. 
 */
StatusCode PMRES::getModelAfterCG() {
  if (!shouldUpdate()) {
    logPrint("ERROR: CG phase proves UNSAT without finding new cores");
  }
  solver = updateSolver();
  setAssumptions(assumptions);
  lbool res; 
  res = searchSATSolver(solver, assumptions);
  assert(res == l_True);

  uint64_t modelCost = computeCostModelPMRES(solver->model);
  assert(modelCost == lbCost);
  if (lbCost < ubCost) {
    ubCost = lbCost;
    saveModel(solver->model);
  }
  printAnswer(_OPTIMUM_);
  return _OPTIMUM_;
}



StatusCode PMRES::onlyLinearSearch() {
  return linearSearch();
  
}


StatusCode PMRES::linearSearch() {
  //NOTE DO NOT COME HERE IMMEDIATELY
  logPrint( "Starting lin search with: LB: " + std::to_string(lbCost) + " UB: " + std::to_string(ubCost) + 
            " UB - LB: " + std::to_string(ubCost-lbCost) + " Time " + print_timeSinceStart() );
  logPrint("REFORM SCLA: " + std::to_string(nRealSoft()));

  inLinSearch = true;
  solver->budgetOff();
  assumptions.clear();


  assert(bestModel.size() > 0);
  savePhase();
  solver->setSolutionBasedPhaseSaving(true);

  
  if(delete_before_lin) {
    solver = resetSolver();
  }
  
  initializeDivisionFactor(varyingres);
  setPBencodings();



  lbool res = l_True;

  while (res == l_True) {

    
    // Do not use preprocessing for linear search algorithm.
    // NOTE: When preprocessing is enabled the SAT solver simplifies the
    // relaxation variables which leads to incorrect results.
    logPrint("SAT Call at " + print_timeSinceStart());
   
    if (!incrementalVarres) {
      assumptions.clear();
    }     
    res = searchSATSolver(solver, assumptions);
    
    
    if (res == l_True) {
      nbSatisfiable++;
      uint64_t new_reduced_cost = computeCostReducedWeights(solver->model);

      if(checkModel()) {
        savePhase();
      }
      if (ubCost == lbCost) {
        if (verbosity > 0)
        printf("c LB = UB\n");
        printAnswer(_OPTIMUM_);
        return _OPTIMUM_;
      }

      if (new_reduced_cost > 0) {
        updateBoundLinSearch(new_reduced_cost - 1);
      }
      else {
        if (maxsat_formula->getMaximumWeight() == 1) {
          printAnswer(_OPTIMUM_);
          return _OPTIMUM_;
        }
        else {
          logPrint("Rebuilding after SAT");
          if (!incrementalVarres) {
            solver = resetSolver();
          } 
          updateDivisionFactorLinear();
          setPBencodings();
        }
      }

    } 
    else {
       if (maxsat_formula->getMaximumWeight() == 1) {
          printAnswer(_OPTIMUM_);
          return _OPTIMUM_;
        }
        else {
          logPrint("Rebuilding after UNSAT");
          if (!incrementalVarres) {
            solver = resetSolver();
          } 
          updateDivisionFactorLinear();
          setPBencodings();
          res = l_True;
        }
      
    }
  }

  return _ERROR_;
}
// Sets according to current maxweight
// Delete solver as needed 
void PMRES::setPBencodings() {
 // assert(!varyingres || maxsat_formula->getMaximumWeight() > 1);
  
  
  uint64_t reduced_cost = computeCostReducedWeights(bestModel); 
  if (reduced_cost == 0 && maxsat_formula->getMaximumWeight() > 1) {
      updateDivisionFactorLinear();
      setPBencodings(); 
      return; 
  }
  logPrint("Building new PB");
  initializePBConstraint(reduced_cost); 
}

void PMRES::updateBoundLinSearch (uint64_t newBound) {
  std::string prefix = maxsat_formula->getProblemType() == _WEIGHTED_ ? "WEIGHTED " : "UNWEIGHTED ";
  
  logPrint("BOUND UPDATE " + prefix + "RHS: " + std::to_string(newBound) + " at " + print_timeSinceStart());
  
  if (maxsat_formula->getProblemType() == _WEIGHTED_) {
    assert(enc->hasPBEncoding());
  } else {
    assert(enc->hasCardEncoding());
  }

  if(!incrementalVarres) {
    if (maxsat_formula->getProblemType() == _WEIGHTED_) {
      enc->updatePB(solver, newBound);
    } else {
      enc->updateCardinality(solver, newBound);
    }
  }
  else {
    assert(maxsat_formula->getProblemType() == _WEIGHTED_ );
    assumptions.clear();
    enc->updatePBA(assumptions, newBound);
  }
} 


void PMRES::initializePBConstraint(uint64_t rhs) {
  initRelaxation();
  if (enc != NULL)
    delete enc;
  enc = new Encoder(_INCREMENTAL_NONE_, _CARD_MTOTALIZER_,
                               _AMO_LADDER_, pb_enc);

  int boundOnVars = solver->nVars();
  if (maxsat_formula->getProblemType() == _WEIGHTED_) {
    assert(!enc->hasPBEncoding());
     
     /*
    int expected_clauses = enc->predictPB(solver, objFunction, coeffs, rhs);
      if (expected_clauses >= _MAX_CLAUSES_) {
        printf("c Warn: changing to Adder encoding.\n");
        logPrint("Clauses: " + std::to_string(expected_clauses) + " max "  + std::to_string(_MAX_CLAUSES_));
        enc->setPBEncoding(_PB_ADDER_);
      } else printf("c GTE auxiliary #clauses = %d\n",expected_clauses);
      */

    logPrint("Encoding PB with UB: " + std::to_string(rhs) );
    enc->encodePB(solver, objFunction, coeffs, rhs  );
    logPrint("Encoding Done");        
  }
  else {
    assert(!enc->hasCardEncoding());
      logPrint("Encoding card with UB: " + std::to_string(rhs) );
      enc->encodeCardinality(solver, objFunction, rhs);
      logPrint("Encoding Done"); 
  }
  setCardVars(boundOnVars);
    
}

void PMRES::initRelaxation() {
  objFunction.clear();
  coeffs.clear();
  uint64_t comW = 0;
  bool unweighted = true; 
  nbCurrentSoft = 0; 

  uint64_t maxreducedweight = 0; 

  for (int i = 0; i < maxsat_formula->nSoft(); i++) {
    uint64_t reducedWeight = maxsat_formula->getSoftClause(i).weight / maxsat_formula->getMaximumWeight();

    if (reducedWeight > 0) { //i.e. if it wasnt hardened in PMRES step OR left out by varres. 
        Lit l = maxsat_formula->getSoftClause(i).assumption_var; 
        assert (l != lit_Undef);
        objFunction.push(l);
        coeffs.push(reducedWeight);
        nbCurrentSoft++;
        if (comW == 0) {
          comW = reducedWeight;
        }
        else {
          if (comW != reducedWeight) {
            unweighted = false;
          }
        }

        if (reducedWeight > maxreducedweight) {
          maxreducedweight = reducedWeight;
        }

    }
  }

  if(incrementalVarres) {
    unweighted = false;
  }

  logPrint("Considering " + std::to_string(nbCurrentSoft) + " of " + std::to_string(nRealSoft()) + " soft clauses");
  
  if (unweighted) {
    logPrint("Unweighted in this iteration");
    maxsat_formula->setProblemType(_UNWEIGHTED_);
  }
  else {
    logPrint("Weighted in this iteration");
    maxsat_formula->setProblemType(_WEIGHTED_);
  }
  
}

void PMRES::setCardVars(int bound) {

    logPrint("Setting Card Vars ");
    solver->setSolutionBasedPhaseSaving(false);
    vec<Lit> cardAssumps;
    assert(bound <= bestModel.size());
    for (int i = 0; i < bound; i++ ) {
      cardAssumps.push(mkLit(i,  bestModel[i] == l_False));
    }
    lbool res = searchSATSolver(solver, cardAssumps);
    if (res == l_False) {
      logPrint("Warning: UNSAT in card setting");
    }
    assert(res == l_True);
    checkModel();
    solver->setSolutionBasedPhaseSaving(true);
    savePhase();
    logPrint("CardVars DONE  ");
    
    assumptions.clear();

}

void PMRES::getModel(vec<lbool> &currentModel, vec<lbool> &fullModel) {
    vec<Lit> modelAssumps;
    for (int i = 0; i < currentModel.size(); i++ ) {
      modelAssumps.push(mkLit(i,  currentModel[i] == l_False));
    }
    solver->setSolutionBasedPhaseSaving(false);
    lbool res = searchSATSolver(solver, modelAssumps);
    solver->setSolutionBasedPhaseSaving(true);
    assert(res == l_True);
    checkModel();
    solver->model.copyTo(fullModel);
}

 

uint64_t PMRES::computeCostReducedWeights(vec<lbool> &inputModel) {
  assert(inputModel.size() != 0);
  assert( maxsat_formula->getSoftClause(maxsat_formula->nSoft() - 1).clause.size() == 1);
  
  vec<lbool> fullModel;
  bool haveModel = var(maxsat_formula->getSoftClause(maxsat_formula->nSoft() - 1).clause[0]) < inputModel.size();
  if (!haveModel) {
    logPrint("UUPS, no model");
    getModel(inputModel, fullModel);
  }
  else {
    inputModel.copyTo(fullModel);
  }

  assert(var(maxsat_formula->getSoftClause(maxsat_formula->nSoft() - 1).clause[0]) < fullModel.size());

  uint64_t tot_reducedCost = 0;

  for (int i = 0; i < maxsat_formula->nSoft(); i++) {
    bool unsatisfied = true;
    assert(maxsat_formula->getSoftClause(i).clause.size() == 1);
    assert(var(maxsat_formula->getSoftClause(i).clause[0]) < fullModel.size());
    if ((sign(maxsat_formula->getSoftClause(i).clause[0]) &&
           fullModel[var(maxsat_formula->getSoftClause(i).clause[0])] ==
               l_False) ||
          (!sign(maxsat_formula->getSoftClause(i).clause[0]) &&
           fullModel[var(maxsat_formula->getSoftClause(i).clause[0])] ==
               l_True)) {
                unsatisfied = false;
          }

    if (unsatisfied) {
      tot_reducedCost += maxsat_formula->getSoftClause(i).weight / maxsat_formula->getMaximumWeight();
    }
  }
  logPrint("Reduced cost " + std::to_string(tot_reducedCost));
  return tot_reducedCost;
}


// Set assumptions on what soft clauses to consider, saves rebuilding the solver; 
void PMRES::setAssumptions(vec<Lit> &assumps) {
    nbCurrentSoft = 0;
    assumps.clear();
    for (int i = 0; i < softs_added ; i++) {
      assert(maxsat_formula->getSoftClause(i).clause.size() == 1);
      
      bool shouldAdd = 
            (maxsat_formula->getSoftClause(i).weight >= maxsat_formula->getMaximumWeight() && !varyingresCG) ||
            (maxsat_formula->getSoftClause(i).weight / maxsat_formula->getMaximumWeight()  > 0 && varyingresCG); 

      if (shouldAdd) {
        Lit l = maxsat_formula->getSoftClause(i).assumption_var;
        assert(l != lit_Undef); 
        assumps.push(~l);
        nbCurrentSoft++;     
      }
    }
  }



// Public search method
StatusCode PMRES::search() {
  if (weightStrategy == _WEIGHT_NONE_) {
    logPrint("forcing a weight strategy on you :)");
    weightStrategy = _WEIGHT_NORMAL_;
  }

  logPrint("PMRES ALGORITHM ");
  logPrint("PMRES LINEAR STRAT=" + std::to_string(lins));
  logPrint("PMRES LINEAR DIVISION=" + std::to_string(varyingres));
  logPrint("PMRES CORE DIVISION=" + std::to_string(varyingresCG));
  logPrint("PMRES CORE LIMIT (-1 = no limit)=" + std::to_string(timeLimitCores));
  logPrint("PMRES RELAX BEFORE STRAT =" + std::to_string(relaxBeforeStrat));
  logPrint("PMRES INCREMENTAL LIN DIVISION =" + std::to_string(incrementalVarres));
  


  costComputingFormula = maxsat_formula;

  maxsat_formula = standardizeMaxSATFormula();
  maxw_nothardened = maxsat_formula->getSumWeights();

  time_start = time(NULL);
	time_best_solution = time_start;

  if (lins == 2) {
    inLinSearch = true;
  }

  logPrint("INIT SCLA: " + std::to_string(nRealSoft()));

  if (setup() == _UNSATISFIABLE_) {
         logPrint("Error: No solutions for instance");
         return _UNSATISFIABLE_;
  }


  switch (lins) {
    case 0:
      return weightSearch();
      break;
    
    case 1:
      return coreGuidedLinearSearch();
      break;

    case 2: 
      return onlyLinearSearch();
      break;
    

    default:
      printf("c Error: Invalid variation value.\n");
      printf("s UNKNOWN\n");
      exit(_ERROR_);
    }
}

/************************************************************************************************
 //
 // Other protected methods
 //
 ************************************************************************************************/

/*_________________________________________________________________________________________________
  |
  |  initAssumptions : (assumps : vec<Lit>&) ->  [void]
  |
  |  Description:
  |
  |    Creates a new assumption literal for each soft clause and initializes the
  |    assumption vector with negation of this literal. Assumptions are used to
  |    extract cores.
  |    TODO: Translate to standard form
  |  Post-conditions:
  |    * For each soft clause 'i' creates an assumption literal and assigns it
  |      to 'softClauses[i].assumptionVar'.
  |    * 'coreMapping' is updated by mapping each assumption literal with the
  |      corresponding index of each soft clause.
  |    * original weights tracks the initial weights of the formula
  |
  |________________________________________________________________________________________________@*/
void PMRES::initAssumptions() {
  for (int i = 0; i < maxsat_formula->nSoft(); i++) {
    assert(maxsat_formula->getSoftClause(i).clause.size() == 1);
    Lit l = maxsat_formula->getSoftClause(i).clause[0];
    maxsat_formula->getSoftClause(i).assumption_var = ~l;
    coreMapping[~l] = i;    
  }
}

void PMRES::logPrint(std::string s) {
  if (verbosity > 0) {
    std::cout << "c " << s << std::endl;
  }
}

void PMRES::printProgress() {
  std::string prefix = inLinSearch ? "LIN " : "CG ";
    logPrint(prefix + "best " + std::to_string(ubCost) + " LB: " + std::to_string(lbCost)  + " at " + std::to_string(time_best_solution - time_start) );
}

std::string PMRES::print_timeSinceStart() {
  return std::to_string(timeSinceStart());
}

time_t PMRES::timeSinceStart() {
  time_t cur = time(NULL);
  return cur - time_start;
}

MaxSATFormula *PMRES::standardizeMaxSATFormula() {
  MaxSATFormula *copymx = new MaxSATFormula();
  copymx->setInitialVars(maxsat_formula->nVars());

  for (int i = 0; i < maxsat_formula->nVars(); i++)
    copymx->newVar();

  for (int i = 0; i < maxsat_formula->nHard(); i++)
    copymx->addHardClause(maxsat_formula->getHardClause(i).clause);

  vec<Lit> clause; 
  for (int i = 0; i < maxsat_formula->nSoft(); i++) {
    clause.clear();
    maxsat_formula->getSoftClause(i).clause.copyTo(clause);
    Lit l = copymx->newLiteral();
    clause.push(l);
    copymx->addHardClause(clause);
    
    clause.clear();
    clause.push(~l);
    copymx->addSoftClause(maxsat_formula->getSoftClause(i).weight, clause);
  }

  copymx->setProblemType(maxsat_formula->getProblemType());
  copymx->updateSumWeights(maxsat_formula->getSumWeights());
  copymx->setMaximumWeight(maxsat_formula->getMaximumWeight());
  copymx->setHardWeight(maxsat_formula->getHardWeight());

  return copymx;
}

void PMRES::addSoftClauseAndAssumptionVar(uint64_t weight, vec<Lit> &clause) {
    Lit l = maxsat_formula->newLiteral();
    clause.push(l);
    maxsat_formula->addHardClause(clause);

    clause.clear();
    clause.push(~l);
    maxsat_formula->addSoftClause(weight, clause);

    maxsat_formula->getSoftClause(maxsat_formula->nSoft() - 1).assumption_var = l;
    coreMapping[l] = maxsat_formula->nSoft() - 1;  // Map the new soft clause to its assumption literal.
}

int PMRES::nRealSoft() {
  return maxsat_formula->nSoft() - num_hardened;
}  

uint64_t PMRES::computeCostModelPMRES(vec<lbool> &currentModel) {
  
  assert(currentModel.size() != 0);
  uint64_t currentCost = 0;

  for (int i = 0; i < costComputingFormula->nSoft(); i++) {
    bool unsatisfied = true;
    for (int j = 0; j < costComputingFormula->getSoftClause(i).clause.size(); j++) {

      assert(var(costComputingFormula->getSoftClause(i).clause[j]) <
             currentModel.size());
      if ((sign(costComputingFormula->getSoftClause(i).clause[j]) &&
           currentModel[var(costComputingFormula->getSoftClause(i).clause[j])] ==
               l_False) ||
          (!sign(costComputingFormula->getSoftClause(i).clause[j]) &&
           currentModel[var(costComputingFormula->getSoftClause(i).clause[j])] ==
               l_True)) {
        unsatisfied = false;
        break;
      }
    }

    if (unsatisfied) {
      currentCost += costComputingFormula->getSoftClause(i).weight;
    }
  }

  return currentCost;
}

bool PMRES::literalTrueInModel(Lit l, vec<lbool> &model) {
  if (var(l) >= model.size()) {
    logPrint("Error, asking for truthness of literal beyond model size");
    assert(var(l) < model.size());
  }

  return (sign(l) && model[var(l)] == l_False) || (!sign(l) && model[var(l)] == l_True);
}



Solver * PMRES::resetSolver() {
    logPrint("Deleting solver");
    delete solver; 
    solver = newSATSolver();
    clauses_added = 0;
    softs_added = 0;
    vars_added = 0;

    return updateSolver();
} 

bool PMRES::shouldUpdate() {
  return clauses_added < maxsat_formula->nHard();
}

// save polarity from last model 
 void PMRES::savePhase() {
    solver->_user_phase_saving.clear();
		for (int i = 0; i < bestModel.size(); i++){
			solver->_user_phase_saving.push(bestModel[i]);		
		}
 }

 void PMRES::softsSatisfied() {
     for (int i = 0; i < costComputingFormula->nSoft(); i++) {
        assert(maxsat_formula->getSoftClause(i).clause.size() == 1 );
        Lit l =  maxsat_formula->getSoftClause(i).clause[0];
        solver->setPolarity(var(l), sign(l) ? true : false);
    }
 }

 bool PMRES::checkModel() {
   uint64_t modelCost = computeCostModelPMRES(solver->model);
   bool isBetter = modelCost < ubCost;
   if (isBetter) {
        ubCost = modelCost;
        time_best_solution = time(NULL);
        printProgress();
        saveModel(solver->model);
        bestModel.clear();
        solver->model.copyTo(bestModel);
        printBound(ubCost);
        checkGap();
    }
    if ( (modelCost == ubCost) && solver->model.size() > bestModel.size()) {
      logPrint("SAME COST BUT LONGER");
      saveModel(solver->model);
      bestModel.clear();
      solver->model.copyTo(bestModel);

    }
    return isBetter;
 }

 void PMRES::checkGap() {
   uint64_t currentGap = ubCost - lbCost;
   if (currentGap < known_gap) {
     known_gap = currentGap;
     if (inLinSearch)
        logPrint("LIN GAP: " + std::to_string(known_gap) + " T " + print_timeSinceStart());
     else 
        logPrint("CG GAP: " + std::to_string(known_gap) + " T " + print_timeSinceStart());
   }
 }
