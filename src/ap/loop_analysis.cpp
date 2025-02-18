/**
 * autoParallelization Project:
 * https://en.wikibooks.org/wiki/ROSE_Compiler_Framework/autoPar#Alternative
 *
 * autoParSupport.cpp from rose/projects/autoParallelization
 * Originally by Chunhua Liao
 *
 *
 * -----------------------------------------------------------
 * Refactored and modified by Lichen Liu
 *
 */
#include "rose.h"
#include "loop_analysis.h"
#include <algorithm> // for set union, intersection etc.
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <unordered_set>
#include "RoseAst.h"
#include "LivenessAnalysis.h"
#include "DefUseAnalysis.h" // Variable classification support
#include "config.hpp"
#include "utils.h"
// Dependence graph headers
#include <AnnotCollect.h>
#include <OperatorAnnotation.h>

namespace
{
    std::unique_ptr<DFAnalysis> l_defuse;
    std::unique_ptr<LivenessAnalysis> l_liv;
}

// Everything should go into the name space here!!
namespace AutoParallelization
{
    bool initialize_analysis(SgProject *project /*=nullptr*/, bool debug /*=false*/)
    {
        if (project == nullptr)
            project = SageInterface::getProject();

        // Prepare def-use analysis
        if (l_defuse == nullptr)
        {
            ROSE_ASSERT(project != nullptr);
            l_defuse = std::make_unique<DefUseAnalysis>(project);
        }

        ROSE_ASSERT(l_defuse != nullptr);
        l_defuse->run(debug);

        if (debug)
            l_defuse->dfaToDOT();

        // Prepare variable liveness analysis
        if (l_liv == nullptr)
            l_liv = std::make_unique<LivenessAnalysis>(debug, (DefUseAnalysis *)l_defuse.get());
        ROSE_ASSERT(l_liv != nullptr);

        std::vector<FilteredCFGNode<IsDFAFilter>> dfaFunctions;
        std::vector<SgFunctionDefinition *> vars =
            SageInterface::querySubTree<SgFunctionDefinition>(project, V_SgFunctionDefinition);
        bool abortme = false;
        // run liveness analysis on each function body
        for (SgFunctionDefinition *func : vars)
        {
            if (debug)
            {
                std::string name = func->class_name();
                std::string funcName = func->get_declaration()->get_qualified_name().str();
                std::cout << " .. running liveness analysis for function: " << funcName << std::endl;
            }
            FilteredCFGNode<IsDFAFilter> rem_source = l_liv->run(func, abortme);
            if (rem_source.getNode() != nullptr)
                dfaFunctions.push_back(rem_source);
            if (abortme)
                break;
        } // end for ()
        if (debug)
        {
            std::cout << "Writing out liveness analysis results into var.dot... " << std::endl;
            std::ofstream f2("var.dot");
            dfaToDot(f2, "var", dfaFunctions, (DefUseAnalysis *)l_defuse.get(), l_liv.get());
            f2.close();
        }
        if (abortme)
        {
            std::cerr << "Error: Liveness analysis is ABORTING ." << std::endl;
            ROSE_ASSERT(false);
        }
        return !abortme;
    } // end initialize_analysis()

    void release_analysis()
    {
        l_defuse.reset(nullptr);
        l_liv.reset(nullptr);
    }

    // Compute dependence graph for a loop, using ArrayInterface and ArrayAnnoation
    //  TODO generate dep graph for the entire function and reuse it for all loops
    LoopTreeDepGraph *ComputeDependenceGraph(SgNode *loop, ArrayInterface *array_interface, ArrayAnnotation *annot)
    {
        ROSE_ASSERT(loop && array_interface && annot);
        // TODO check if its a canonical loop

        // Prepare AstInterface: implementation and head pointer
        AstInterfaceImpl faImpl_2 = AstInterfaceImpl(loop);
        // AstInterface fa(&faImpl); // Using CPP interface to handle templates etc.
        CPPAstInterface fa(&faImpl_2);
        AstNodePtr head = AstNodePtrImpl(loop);
        // AstNodePtr head = AstNodePtrImpl(body);
        fa.SetRoot(head);

        LoopTransformInterface::set_astInterface(fa);
        LoopTransformInterface::set_arrayInfo(array_interface);
        LoopTransformInterface::set_aliasInfo(array_interface);
        LoopTransformInterface::set_sideEffectInfo(annot);
        LoopTreeDepCompCreate *comp = new LoopTreeDepCompCreate(head, true, true);
        // the third parameter sets supportNonFortranLoop to true
        // TODO when to release this?
        // Retrieve dependence graph here!
        if (AP::Config::get().enable_debug)
        {
            SgStatement *stmt = isSgStatement(loop);
            ROSE_ASSERT(stmt != nullptr);
            std::cout << "START ComputeDependenceGraph() dumps the dependence graph for the loop at line :" << stmt->get_file_info()->get_line() << std::endl;
            comp->DumpDep();
            std::cout << "END ComputeDependenceGraph()" << std::endl;
        }

        // The following code was used when an entire function body with several loops
        // is analyzed for dependence analysis. I keep it to double check the computation.

        // Get the loop hierarchy :grab just a top one for now
        // TODO consider complex loop nests like loop {loop, loop} and loop{loop {loop}}
        LoopTreeNode *loop_root = comp->GetLoopTreeRoot();
        ROSE_ASSERT(loop_root != nullptr);
        // loop_root->Dump();
        LoopTreeTraverseSelectLoop loop_nodes(loop_root, LoopTreeTraverse::PreOrder);
        LoopTreeNode *cur_loop = loop_nodes.Current();
        // three-level loop: i,j,k
        AstNodePtr ast_ptr;
        if (cur_loop)
        {
            // cur_loop->Dump();
            // loop_nodes.Advance();
            // loop_nodes.Current()->Dump();
            // loop_nodes.Advance();
            // loop_nodes.Current()->Dump();
            ast_ptr = dynamic_cast<LoopTreeLoopNode *>(cur_loop)->GetOrigLoop();
            // std::cout<<AstToString(ast_ptr)<<std::endl;
            ROSE_ASSERT(ast_ptr != nullptr);
            SgNode *sg_node = AstNodePtr2Sage(ast_ptr);
            ROSE_ASSERT(sg_node == loop);
            // std::cout<<"-------------Dump the loops in question------------"<<std::endl;
            //   std::cout<<sg_node->class_name()<<std::endl;
            return comp->GetDepGraph();
        }
        else
        {
            std::cout << "Skipping a loop not recognized by LoopTreeTraverseSelectLoop ..." << std::endl;
            return nullptr;
            // Not all loop can be collected by LoopTreeTraverseSelectLoop right now
            // e.g: loops in template function bodies
            // ROSE_ASSERT(false);
        }
    }

    // Get the live-in and live-out variable sets for a for loop,
    // recomputing liveness analysis if requested (useful after program transformation)
    // Only consider scalars for now, ignore non-scalar variables
    // Also ignore loop invariant variables.
    void GetLiveVariables(SgNode *loop, std::vector<SgInitializedName *> &liveIns,
                          std::vector<SgInitializedName *> &liveOuts, bool reCompute /*=false*/)
    {
        // TODO reCompute : call another liveness analysis function on a target function
        if (reCompute)
            initialize_analysis();

        std::vector<SgInitializedName *> liveIns0, liveOuts0; // store the original one
        SgInitializedName *invarname = getLoopInvariant(loop);
        // Grab the filtered CFG node for SgForStatement
        SgForStatement *forstmt = isSgForStatement(loop);
        ROSE_ASSERT(forstmt);
        // Jeremiah's hidden constructor to grab the right one
        // Several CFG nodes are used for the same SgForStatement
        CFGNode cfgnode(forstmt, 2);
        FilteredCFGNode<IsDFAFilter> filternode = FilteredCFGNode<IsDFAFilter>(cfgnode);
        // This one does not return the one we want even its getNode returns the
        // right for statement
        // FilteredCFGNode<IsDFAFilter> filternode= FilteredCFGNode<IsDFAFilter> (forstmt->cfgForBeginning());
        ROSE_ASSERT(filternode.getNode() == forstmt);

        // Check out edges
        std::vector<FilteredCFGEdge<IsDFAFilter>> out_edges = filternode.outEdges();
        // std::cout<<"Found edge count:"<<out_edges.size()<<std::endl;
        // SgForStatement should have two outgoing edges, one true(going into the loop body) and one false (going out the loop)
        ROSE_ASSERT(out_edges.size() == 2);
        //  std::vector<SgInitializedName*> remove1, remove2;
        for (const auto &edge : out_edges)
        {
            // Used to verify CFG nodes in var.dot dump
            // x. Live-in (loop) = live-in (first-stmt-in-loop)
            if (edge.condition() == eckTrue)
            {
                SgNode *firstnode = edge.target().getNode();
                liveIns0 = l_liv->getIn(firstnode);
                if (AP::Config::get().enable_debug)
                    std::cout << "Live-in variables for loop:" << firstnode->get_file_info()->get_line() << std::endl;
                for (auto name : liveIns0)
                {
                    if ((SageInterface::isScalarType(name->get_type())) && (name != invarname))
                    {
                        liveIns.push_back(name);
                        if (AP::Config::get().enable_debug)
                            std::cout << "  " << name->get_qualified_name().getString() << std::endl;
                    }
                }
            }
            // x. live-out(loop) = live-in (first-stmt-after-loop)
            else if (edge.condition() == eckFalse)
            {
                SgNode *firstnode = edge.target().getNode();
                liveOuts0 = l_liv->getIn(firstnode);
                if (AP::Config::get().enable_debug)
                    std::cout << "Live-out variables for loop before line:" << firstnode->get_file_info()->get_line() << std::endl;
                for (auto name : liveOuts0)
                {
                    if ((SageInterface::isScalarType(name->get_type())) && (name != invarname))
                    {
                        if (AP::Config::get().enable_debug)
                            std::cout << "  " << name->get_qualified_name().getString() << std::endl;
                        liveOuts.push_back(name);
                    }
                }
            }
            else
            {
                std::cerr << "Unexpected CFG out edge type for SgForStmt!" << std::endl;
                ROSE_ASSERT(false);
            }
        } // end for (edges)
        // debug the final results
        if (AP::Config::get().enable_debug)
        {
            std::cout << "Final Live-in variables for loop:" << std::endl;
            for (auto name : liveIns)
            {
                std::cout << "  " << name->get_qualified_name().getString() << std::endl;
            }
            std::cout << "Final Live-out variables for loop:" << std::endl;
            for (auto name : liveOuts)
            {
                std::cout << "  " << name->get_qualified_name().getString() << std::endl;
            }
        }

    } // end GetLiveVariables()

    // Check if a loop has a canonical form, which has
    //  * initialization statements;
    //  * a test expression  using either <= or >= operations
    //  * an increment expression using i=i+1, or i=i-1.
    // If yes, grab its invariant, lower bound, upper bound, step, and body if requested
    // Return the loop invariant of a canonical loop
    // Return nullptr if the loop is not canonical
    SgInitializedName *getLoopInvariant(SgNode *loop)
    {
        // Qing's IsFortranLoop does not check the structured block requirement
        // We use our own isCanonicalLoop instead.
        SgInitializedName *invarname = nullptr;
        if (!SageInterface::isCanonicalForLoop(loop, &invarname))
            return nullptr;

        return invarname;
    }

    // Collect sorted and unique visible referenced variables within a scope.
    // ignoring loop invariant and local variables declared within the scope.
    // They are less interesting for auto parallelization
    void CollectVisibleVaribles(SgNode *loop, std::vector<SgInitializedName *> &resultVars, std::vector<SgInitializedName *> &invariantVars, bool scalarOnly /*=false*/)
    {
        ROSE_ASSERT(loop != nullptr);
        // Get the scope of the loop

        SgScopeStatement *currentscope = SageInterface::getEnclosingNode<SgScopeStatement>(loop, false);
        ROSE_ASSERT(currentscope != nullptr);

        SgInitializedName *invarname = getLoopInvariant(loop);
        std::vector<SgVarRefExp *> reflist = SageInterface::querySubTree<SgVarRefExp>(loop, V_SgVarRefExp);
        for (SgVarRefExp *varRef : reflist)
        {
            SgInitializedName *initname = varRef->get_symbol()->get_declaration();
            SgScopeStatement *varscope = initname->get_scope();
            // only collect variables which are visible at the loop's scope
            // varscope is equal or higher than currentscope
            if ((currentscope == varscope) || (SageInterface::isAncestor(varscope, currentscope)))
            {
                // Skip non-scalar if scalarOnly is requested
                if ((scalarOnly) && !SageInterface::isScalarType(initname->get_type()))
                    continue;
                if (invarname != initname)
                    resultVars.push_back(initname);
            }
        } // end for()

        // collect loop invariants here
        std::vector<SgForStatement *> loopnests = SageInterface::querySubTree<SgForStatement>(loop, V_SgForStatement);
        for (SgForStatement *forstmt : loopnests)
        {
            SgInitializedName *invariant = getLoopInvariant(forstmt);
            if (invariant)
            {
                SgScopeStatement *varscope = invariant->get_scope();
                // only collect variables which are visible at the loop's scope
                // varscope is equal or higher than currentscope
                if ((currentscope == varscope) || (SageInterface::isAncestor(varscope, currentscope)))
                    invariantVars.push_back(invariant);
            }
        }

        // Remove duplicated items
        std::sort(resultVars.begin(), resultVars.end());
        resultVars.erase(std::unique(resultVars.begin(), resultVars.end()), resultVars.end());

        std::sort(invariantVars.begin(), invariantVars.end());
        invariantVars.erase(std::unique(invariantVars.begin(), invariantVars.end()), invariantVars.end());
    }

    //! Collect a loop's variables which cause any kind of dependencies. Consider scalars only if requested.
    // depgraph may contain dependencies for the entire function enclosing the loop. So we need to ignore irrelevant ones with respect to the loop
    void CollectVariablesWithDependence(SgNode *loop, LoopTreeDepGraph *depgraph, std::vector<SgInitializedName *> &resultVars, bool scalarOnly /*=false*/)
    {
        ROSE_ASSERT(isSgForStatement(loop) && depgraph);
        LoopTreeDepGraph::NodeIterator nodes = depgraph->GetNodeIterator();
        // For each node
        for (; !nodes.ReachEnd(); ++nodes)
        {
            LoopTreeDepGraph::Node *curnode = *nodes;
            LoopTreeDepGraph::EdgeIterator edges = depgraph->GetNodeEdgeIterator(curnode, GraphAccess::EdgeOut);
            // If the node has edges
            if (!edges.ReachEnd())
            {
                // for each edge
                for (; !edges.ReachEnd(); ++edges)
                {
                    LoopTreeDepGraph::Edge *e = *edges;
                    // std::cout<<"dependence edge: "<<e->toString()<<std::endl;
                    DepInfo info = e->GetInfo();
                    // Indicate if the variable references happen within the loop
                    bool insideLoop1 = false, insideLoop2 = false;

                    SgScopeStatement *loopscope = SageInterface::getScope(loop);
                    SgScopeStatement *varscope = nullptr;
                    SgNode *src_node = AstNodePtr2Sage(info.SrcRef());
                    SgInitializedName *src_name = nullptr;
                    if (src_node)
                    { // TODO May need to consider a wider concept of variable reference
                        // like AstInterface::IsVarRef()
                        SgVarRefExp *var_ref = isSgVarRefExp(src_node);
                        if (var_ref)
                        {
                            varscope = var_ref->get_symbol()->get_scope();
                            src_name = var_ref->get_symbol()->get_declaration();
                            // Ignore the local variables declared inside the loop
                            if (SageInterface::isAncestor(loopscope, varscope))
                                continue;
                            if (SageInterface::isAncestor(loopscope, var_ref))
                                insideLoop1 = true;
                        } // end if(var_ref)
                    }     // end if (src_node)
                    SgNode *snk_node = AstNodePtr2Sage(info.SnkRef());
                    SgInitializedName *snk_name = nullptr;
                    if (snk_node)
                    {
                        SgVarRefExp *var_ref = isSgVarRefExp(snk_node);
                        if (var_ref)
                        {
                            varscope = var_ref->get_symbol()->get_scope();
                            snk_name = var_ref->get_symbol()->get_declaration();
                            if (SageInterface::isAncestor(loopscope, varscope))
                                continue;
                            if (SageInterface::isAncestor(loopscope, var_ref))
                                insideLoop2 = true;
                        } // end if(var_ref)
                    }     // end if (snk_node)
                    // Only collect the dependence relation involving
                    // two variables referenced within the loop
                    if (insideLoop1 && insideLoop2)
                    {
                        if (scalarOnly)
                        { // Only meaningful if both are scalars
                            if (SageInterface::isScalarType(src_name->get_type()) && SageInterface::isScalarType(snk_name->get_type()))
                            {
                                resultVars.push_back(src_name);
                                resultVars.push_back(snk_name);
                            }
                        }
                        else
                        {
                            resultVars.push_back(src_name);
                            resultVars.push_back(snk_name);
                        }
                    }
                } // end iterator edges for a node
            }     // end if has edge
        }         // end of iterate dependence graph
        // remove duplicated items
        std::sort(resultVars.begin(), resultVars.end());
        resultVars.erase(std::unique(resultVars.begin(), resultVars.end()), resultVars.end());
    }

    // Variable classification for a loop node based on liveness analysis
    // Collect private, firstprivate, lastprivate, reduction and save into attribute
    // We only consider scalars for now
    // Algorithm: private and reduction variables cause dependences (being written)
    //            firstprivate and lastprivate variables are never being written in the loop (no dependences)
    /*                              live-in      live-out
                     shared            Y           Y      no written, no dependences: no special handling, shared by default
                     private           N           N      written (depVars), need privatization: depVars- liveIns - liveOuts
                     firstprivate      Y           N      liveIns - LiveOuts - writtenVariables
                     lastprivate       N           Y      liveOuts - LiveIns
                     reduction         Y           Y      depVars Intersection (liveIns Intersection liveOuts)
                     */

    void AutoScoping(SgNode *sg_node, OmpSupport::OmpAttribute *attribute, LoopTreeDepGraph *depgraph)
    {
        ROSE_ASSERT(sg_node && attribute && depgraph);
        ROSE_ASSERT(isSgForStatement(sg_node));

        // Variable liveness analysis: original ones and
        // the one containing only variables with some kind of dependencies
        std::vector<SgInitializedName *> liveIns0, liveIns;
        std::vector<SgInitializedName *> liveOuts0, liveOuts;
        // Turn on recomputing since transformations have been done
        // GetLiveVariables(sg_node,liveIns,liveOuts,true);
        // TODO Loop normalization messes up AST or
        // the existing analysis can not be called multiple times
        GetLiveVariables(sg_node, liveIns0, liveOuts0, false);
        // Remove loop invariant variable, which is always private
        SgInitializedName *invarname = getLoopInvariant(sg_node);
        SgForStatement *for_stmt = isSgForStatement(sg_node);
        ROSE_ASSERT(for_stmt != nullptr);

        std::remove(liveIns0.begin(), liveIns0.end(), invarname);
        std::remove(liveOuts0.begin(), liveOuts0.end(), invarname);

        std::vector<SgInitializedName *> allVars, depVars, invariantVars, privateVars, lastprivateVars,
            firstprivateVars, reductionVars; // reductionResults;
        // Only consider scalars for now
        CollectVisibleVaribles(sg_node, allVars, invariantVars, true);
        std::sort(allVars.begin(), allVars.end());
        CollectVariablesWithDependence(sg_node, depgraph, depVars, true);
        if (AP::Config::get().enable_debug)
        {
            std::cout << "Debug after CollectVisibleVaribles ():" << std::endl;
            for (auto name : allVars)
            {
                std::cout << "  " << AP::to_string(name) << std::endl;
            }

            std::cout << "Debug after CollectVariablesWithDependence():" << std::endl;
            for (auto name : depVars)
            {
                std::cout << "  " << AP::to_string(name) << std::endl;
            }
        }
        std::sort(liveIns0.begin(), liveIns0.end());
        std::sort(liveOuts0.begin(), liveOuts0.end());

        // We concern about variables with some kind of dependences
        // Since private and reduction variables cause some kind of dependencies ,
        // which otherwise prevent parallelization
        // liveVars intersection depVars
        // Remove the live variables which have no relevant dependencies
        std::set_intersection(liveIns0.begin(), liveIns0.end(), depVars.begin(), depVars.end(),
                              std::back_inserter(liveIns));
        std::set_intersection(liveOuts0.begin(), liveOuts0.end(), depVars.begin(), depVars.end(),
                              std::back_inserter(liveOuts));

        std::sort(liveIns.begin(), liveIns.end());
        std::sort(liveOuts.begin(), liveOuts.end());

        // shared: scalars for now: allVars - depVars,
        if (AP::Config::get().enable_debug)
        {
            std::cout << "Debug dump shared:" << std::endl;
            std::vector<SgInitializedName *> sharedVars;
            std::set_difference(allVars.begin(), allVars.end(), depVars.begin(), depVars.end(),
                                std::back_inserter(sharedVars));
            for (auto name : sharedVars)
            {
                std::cout << "  " << AP::to_string(name) << std::endl;
            }
        }

        // private:
        //---------------------------------------------
        // depVars- liveIns - liveOuts
        std::vector<SgInitializedName *> temp;
        std::set_difference(depVars.begin(), depVars.end(), liveIns.begin(), liveIns.end(),
                            std::back_inserter(temp));
        std::set_difference(temp.begin(), temp.end(), liveOuts.begin(), liveOuts.end(),
                            std::back_inserter(privateVars));
        // loop invariants are private
        // insert all loops, including nested ones' visible invariants
        privateVars.insert(privateVars.end(), invariantVars.begin(), invariantVars.end());
        if (AP::Config::get().enable_debug)
            std::cout << "Debug dump private:" << std::endl;

        // Get all possible in inner loops normalized loop index variables captured by the scope of the current for_stmt
        std::vector<SgForStatement *> inner_for_stmts = SageInterface::querySubTree<SgForStatement>(for_stmt, V_SgForStatement);
        ROSE_ASSERT(std::count(inner_for_stmts.begin(), inner_for_stmts.end(), for_stmt) == 1);
        std::unordered_set<SgVariableSymbol *> ndecl_syms;
        for (auto inner_for_stmt : inner_for_stmts)
        {
            bool hasNormalization = SageInterface::trans_records.forLoopInitNormalizationTable[inner_for_stmt];
            if (hasNormalization)
            {
                // get the normalization generated declaration
                SgVariableDeclaration *ndecl =
                    SageInterface::trans_records.forLoopInitNormalizationRecord[inner_for_stmt].second;
                ndecl_syms.emplace(SageInterface::getFirstVarSym(ndecl));
            }
        }

        for (auto name : privateVars)
        {
            // Liao 6/22/2016.
            // Loop normalization will convert C99 loop init-stmt into two statements and rename the loop index variable.
            // This causes problem for patch generation since an additional loop variable (e.g. i_norm_1) shows up in the private () clause.
            // To workaround this problem, we skip recording the loop index variable generated by loop normalization.
            std::string var_name = name->get_name().getString();
            bool skipAdd = false;
            // This does not work unless the normalized c99 init-stmt is undone!!
            // The same attribute is used for both patch generation and code generation!
            // Even worse, a previous workaround changed the name convention of normalized loop variables:
            // The name will be kept the same when possible to have cosmetic improvements.
            // https://github.com/rose-compiler/rose-develop/commit/a69182fbbac8d95bf577e3a9d7361bb57d03eb0f
            //
            // To solve this problem, we record loop normalization for c99 init-stmt and use symbol comparison to be sure.
            //
            // Another concern is skipping this loop variable in private() may impact dependence elimination later on.
            // This concern is not valid so far based on testing.

            // this is a variable generated by loop normalization
            // the current rule is originalName_norm_id;
            if (ndecl_syms.count(isSgVariableSymbol(name->search_for_symbol_from_symbol_table())) == 1)
            {
                skipAdd = true;
            }
            if (!skipAdd)
            {
                attribute->addVariable(OmpSupport::e_private, var_name, name);

                if (AP::Config::get().enable_debug)
                {
                    std::cout << "  " << AP::to_string(name) << std::endl;
                }
            }
        }

        // lastprivate: liveOuts - LiveIns
        //  Must be written and LiveOut to have the need to preserve the value:  DepVar Intersect LiveOut
        //  Must not be Livein to ensure correct semantics: private for each iteration, not getting value from previous iteration.
        //   e.g.  for ()   {  a = 1; }  = a;
        //---------------------------------------------
        std::set_difference(liveOuts.begin(), liveOuts.end(), liveIns0.begin(), liveIns0.end(),
                            std::back_inserter(lastprivateVars));

        if (AP::Config::get().enable_debug)
            std::cout << "Debug dump lastprivate:" << std::endl;
        for (auto name : lastprivateVars)
        {
            attribute->addVariable(OmpSupport::e_lastprivate, name->get_name().getString(), name);
            if (AP::Config::get().enable_debug)
            {
                std::cout << "  " << AP::to_string(name) << std::endl;
            }
        }
        // reduction recognition
        //---------------------------------------------
        // Some 'bad' examples have reduction variables which are not used after the loop
        // So we relax the constrains as liveIns only for reduction variables

        // reductionResults = RecognizeReduction(sg_node,attribute, liveIns);
        //  Using the better SageInterface version , Liao 9/14/2016
        std::set<std::pair<SgInitializedName *, OmpSupport::omp_construct_enum>> reductionResults;
        SageInterface::ReductionRecognition(isSgForStatement(sg_node), reductionResults);
        if (AP::Config::get().enable_debug)
            std::cout << "Debug dump reduction:" << std::endl;
        for (auto [iname, optype] : reductionResults)
        {
            attribute->addVariable(optype, iname->get_name().getString(), iname);
            if (AP::Config::get().enable_debug)
            {
                std::cout << "  " << AP::to_string(iname) << std::endl;
            }
        }

        // Liao 5/28/2010: firstprivate variables should not cause any dependencies, equal to should be be written in the loop
        // firstprivate:  liveIns - LiveOuts - writtenVariables (or depVars)
        //---------------------------------------------
        //     liveIn : the need to pass in value
        //     not liveOut: differ from Shared, we considered shared first, then firstprivate
        //     not written: ensure the correct semantics: each iteration will use a copy from the original master, not redefined
        //                  value from the previous iteration
        if (AP::Config::get().enable_debug)
            std::cout << "Debug dump firstprivate:" << std::endl;

        std::vector<SgInitializedName *> temp2, temp3;
        std::set_difference(liveIns0.begin(), liveIns0.end(), liveOuts0.begin(), liveOuts0.end(),
                            std::back_inserter(temp2));
        std::set_difference(temp2.begin(), temp2.end(), depVars.begin(), depVars.end(),
                            std::back_inserter(temp3));
        // Liao 6/27/2014
        // LiveIn only means may be used, not must be used, in the future.
        // some liveIn variables may not show up at all in the loop body we concern about
        // So we have to intersect with visible variables to make sure we only put used variables into the firstprivate clause
        std::set_intersection(temp3.begin(), temp3.end(), allVars.begin(), allVars.end(), std::back_inserter(firstprivateVars));
        for (auto name : firstprivateVars)
        {
            attribute->addVariable(OmpSupport::e_firstprivate, name->get_name().getString(), name);
            if (AP::Config::get().enable_debug)
            {
                std::cout << "  " << AP::to_string(name) << std::endl;
            }
        }
    } // end AutoScoping()

    std::vector<SgInitializedName *> CollectUnallowedScopedVariables(OmpSupport::OmpAttribute *attribute)
    {
        ROSE_ASSERT(attribute != nullptr);
        std::vector<SgInitializedName *> result;
        // lastprivate, reduction
        std::vector<std::pair<std::string, SgNode *>> lastVars = attribute->getVariableList(OmpSupport::e_lastprivate);
        std::vector<std::pair<std::string, SgNode *>> reductionVars = attribute->getVariableList(OmpSupport::e_reduction);

        for (const auto &[_, name] : lastVars)
        {
            SgInitializedName *initname = isSgInitializedName(name);
            ROSE_ASSERT(initname != nullptr);
            result.push_back(initname);
        }
        for (const auto &[_, name] : reductionVars)
        {
            SgInitializedName *initname = isSgInitializedName(name);
            ROSE_ASSERT(initname != nullptr);
            result.push_back(initname);
        }
        // avoid duplicated items
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    // Collect all classified variables from an OmpAttribute attached to a loop node
    std::vector<SgInitializedName *> CollectAllowedScopedVariables(OmpSupport::OmpAttribute *attribute)
    {
        ROSE_ASSERT(attribute != nullptr);
        std::vector<SgInitializedName *> result;
        // private, firstprivate
        std::vector<std::pair<std::string, SgNode *>> privateVars = attribute->getVariableList(OmpSupport::e_private);
        std::vector<std::pair<std::string, SgNode *>> firstprivateVars = attribute->getVariableList(OmpSupport::e_firstprivate);

        for (const auto &[_, name] : privateVars)
        {
            SgInitializedName *initname = isSgInitializedName(name);
            ROSE_ASSERT(initname != nullptr);
            result.push_back(initname);
        }
        for (const auto &[_, name] : firstprivateVars)
        {
            SgInitializedName *initname = isSgInitializedName(name);
            ROSE_ASSERT(initname != nullptr);
            result.push_back(initname);
        }
        // avoid duplicated items
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    //! Check if a reference is an array reference of statically declared arrays
    // SgPntrArrRefExp -> lhs_operand_i() → SgVarRefExp -> SgVariableSymbol -> SgInitializedName → typeptr → SgArrayType
    static bool isStaticArrayRef(SgNode *ref)
    {
        bool ret = false;
        ROSE_ASSERT(ref != nullptr);

        if (SgPntrArrRefExp *aref = isSgPntrArrRefExp(ref))
        {
            // for multidimensional array references, getting the nested child SgPntrArrRef
            if (SgPntrArrRefExp *nestRef = isSgPntrArrRefExp(aref->get_lhs_operand_i()))
                return isStaticArrayRef(nestRef);

            SgVarRefExp *lhs = isSgVarRefExp(aref->get_lhs_operand_i());
            if (lhs != nullptr)
            {
                SgVariableSymbol *varSym = isSgVariableSymbol(lhs->get_symbol());
                if (varSym != nullptr)
                {
                    SgInitializedName *iname = varSym->get_declaration();
                    if (isSgArrayType(iname->get_type()))
                        ret = true;
                }
            }
        }

        return ret;
    }

    // Algorithm, eliminate the following dependencies
    // *  caused by locally declared variables: already private to each iteration
    // *  commonlevel ==0, no common enclosing loops
    // *  carry level !=0, loop independent. This is valid since we run dep analysis on each level of loop.
    //    For a current loop to be considered, only the dependences carried by the current loop matters.
    //    The current loop is always the outermost level loop , carry level id is 0.
    // *  either source or sink variable is thread local variable
    // *  dependencies caused by autoscoped variables (private, firstprivate, lastprivate, reduction)
    // *  two array references, but SCALAR_DEP or SCALAR_BACK_DEP dependencies
    // OmpAttribute provides scoped variables
    // ArrayInterface and ArrayAnnotation support optional annotation based high level array abstractions
    void DependenceElimination(SgNode *sg_node, LoopTreeDepGraph *depgraph, std::vector<DepInfo> &remainings, OmpSupport::OmpAttribute *att,
                               std::map<SgNode *, bool> &indirect_table, ArrayInterface *array_interface /*=0*/, ArrayAnnotation *annot /*=0*/)
    {
        // LoopTreeDepGraph * depgraph =  comp.GetDepGraph();
        LoopTreeDepGraph::NodeIterator nodes = depgraph->GetNodeIterator();
        if (AP::Config::get().enable_debug)
        {
            std::cout << "Entering DependenceElimination ()" << std::endl;
        }
        // For each node
        for (; !nodes.ReachEnd(); ++nodes)
        {
            LoopTreeDepGraph::Node *curnode = *nodes;
            LoopTreeDepGraph::EdgeIterator edges = depgraph->GetNodeEdgeIterator(curnode, GraphAccess::EdgeOut);
            // If the node has edges
            if (!edges.ReachEnd())
            {
                // for each edge
                for (; !edges.ReachEnd(); ++edges)
                {
                    LoopTreeDepGraph::Edge *e = *edges;
                    DepInfo info = e->GetInfo();
                    if (AP::Config::get().enable_debug)
                        std::cout << "-------------->>> Considering a new dependence edge's info:\n"
                                  << info.toString() << std::endl;

                    SgScopeStatement *currentscope = SageInterface::getScope(sg_node);
                    SgScopeStatement *varscope = nullptr;
                    SgNode *src_node = AstNodePtr2Sage(info.SrcRef());
                    SgInitializedName *src_name = nullptr;
                    // two variables will be set if source or snk nodes are variable references nodes
                    SgVarRefExp *src_var_ref = nullptr;
                    SgVarRefExp *snk_var_ref = nullptr;

                    // x. Ignore dependence caused by locally declared variables: declared within the loop
                    if (src_node)
                    {
                        SgVarRefExp *var_ref = isSgVarRefExp(src_node);
                        if (var_ref)
                        {
                            src_var_ref = var_ref;
                            varscope = var_ref->get_symbol()->get_scope();
                            src_name = var_ref->get_symbol()->get_declaration();
                            if (SageInterface::isAncestor(currentscope, varscope))
                            {
                                if (AP::Config::get().enable_debug)
                                {
                                    std::cout << "Eliminating a dep relation due to locally declared src variable" << std::endl;
                                    info.Dump();
                                }
                                continue;
                            }
                        } // end if(var_ref)
                    }     // end if (src_node)

                    SgNode *snk_node = AstNodePtr2Sage(info.SnkRef());
                    SgInitializedName *snk_name = nullptr;
                    // x. Eliminate dependence relationship if one of the pair is thread local
                    // -----------------------------------------------
                    // either of the source or sink variables are thread-local:
                    // (within the scope of the loop's scope)
                    // There is no loop carried dependence in this case
                    if (snk_node)
                    {
                        SgVarRefExp *var_ref = isSgVarRefExp(snk_node);
                        snk_var_ref = var_ref;
                        if (var_ref)
                        {
                            varscope = var_ref->get_symbol()->get_scope();
                            snk_name = var_ref->get_symbol()->get_declaration();
                            if (SageInterface::isAncestor(currentscope, varscope))
                            {
                                if (AP::Config::get().enable_debug)
                                {
                                    std::cout << "Eliminating a dep relation due to locally declared sink variable" << std::endl;
                                    info.Dump();
                                }
                                continue;
                            }
                        } // end if(var_ref)
                    }     // end if (snk_node)
                    if (AP::Config::get().enable_debug)
                        std::cout << "Neither source nor sink node is locally decalared variables." << std::endl;

                    // x. Eliminate a dependence if it is empty entry
                    //  -----------------------------------------------
                    //  Ignore possible empty depInfo entry
                    if (src_node == nullptr || snk_node == nullptr)
                    {
                        if (AP::Config::get().enable_debug)
                        {
                            std::cout << "Eliminating a dep relation due to empty entry for either src or sink variables or both" << std::endl;
                            info.Dump();
                        }
                        continue;
                    }

                    if (AP::Config::get().enable_debug)
                        std::cout << "Neither source nor sink node is empty entry." << std::endl;

                    // x. Eliminate a dependence if scalar type dependence involving array references.
                    //  -----------------------------------------------
                    //  At least one of the source and sink variables are array references (not scalar)
                    //  But the dependence type is scalar type
                    //    * array-to-array, but scalar type dependence
                    //    * scalar-to-array dependence.
                    //  RISKY!!   We essentially assume no aliasing between arrays and scalars here!!
                    //     I cannot think of a case in which a scalar and array element can access the same memory location otherwise.
                    //  According to Qing:
                    //    A scalar dep is simply the dependence between two scalar variables.
                    //    There is no dependence between a scalar variable and an array variable.
                    //    The GlobalDep function simply computes dependences between two scalar
                    //    variable references (to the same variable)
                    //    inside a loop, and the scalar variable is not considered private.
                    //  We have autoscoping to take care of scalars, so we can safely skip them
                    bool isArray1 = false, isArray2 = false;
                    AstInterfaceImpl faImpl = AstInterfaceImpl(sg_node);
                    AstInterface fa(&faImpl);
                    // If we have array annotation, use loop transformation interface's IsArrayAccess()
                    if (array_interface && annot)
                    {
                        LoopTransformInterface::set_astInterface(fa);
                        LoopTransformInterface::set_arrayInfo(array_interface);
                        LoopTransformInterface::set_sideEffectInfo(annot);

                        isArray1 = LoopTransformInterface::IsArrayAccess(info.SrcRef());
                        isArray2 = LoopTransformInterface::IsArrayAccess(info.SnkRef());
                    }
                    else // use AstInterface's IsArrayAccess() otherwise
                    {
                        isArray1 = fa.IsArrayAccess(info.SrcRef());
                        isArray2 = fa.IsArrayAccess(info.SnkRef());
                    }

                    // if (isArray1 && isArray2) // changed from both to either to be aggressive, 5/25/2010
                    if (isArray1 || isArray2)
                    {
                        if (AP::Config::get().enable_debug)
                            std::cout << "Either source or sink reference is an array reference..." << std::endl;

                        if ((info.GetDepType() & DEPTYPE_SCALAR) || (info.GetDepType() & DEPTYPE_BACKSCALAR))
                        {
                            if (AP::Config::get().enable_debug)
                                std::cout << "\t Dep type is scalar or backscalar " << std::endl;
                            if (src_var_ref || snk_var_ref) // at least one is a scalar: we have scalar vs. array
                            {
                                if (AP::Config::get().enable_debug)
                                    std::cout << "Either source or sink reference is a scalar reference..." << std::endl;
                                // we have to check the type of the scalar:
                                //  integer type? skip
                                //  pointer type, skip if no-aliasing is specified
                                SgVarRefExp *one_var = src_var_ref ? src_var_ref : snk_var_ref;

                                // non-pointer type or pointertype && no_aliasing, we skip it
                                if (!SageInterface::isPointerType(one_var->get_type()) || AP::Config::get().no_aliasing)
                                {
                                    if (AP::Config::get().enable_debug)
                                    {
                                        if (AP::Config::get().no_aliasing)
                                            std::cout << "Non-aliasing assumed, eliminating a dep relation due to scalar dep type for at least one array variable (pointers used as arrays)" << std::endl;
                                        else
                                            std::cout << "Found a non-pointer scalar, eliminating a dep relation due to the scalar dep type between a scalar and an array" << std::endl;
                                        info.Dump();
                                    }

                                    continue;
                                }
                            }
                            else // both are arrays
                            {
                                if (AP::Config::get().enable_debug)
                                    std::cout << "\t both are arrray references " << std::endl;
                                if (AP::Config::get().no_aliasing)
                                {
                                    if (AP::Config::get().enable_debug)
                                    {
                                        std::cout << "Non-aliasing assumed, eliminating a dep relation due to scalar dep type for at least one array variable (pointers used as arrays)" << std::endl;
                                        info.Dump();
                                    }
                                    continue;
                                }
                                // both are arrays and both are statically allocated ones
                                else if (isStaticArrayRef(src_node) && isStaticArrayRef(snk_node))
                                {
                                    if (AP::Config::get().enable_debug)
                                    {
                                        std::cout << "Eliminating a dep relation due to both references are references to static allocated arrays " << std::endl;
                                        info.Dump();
                                    }
                                    continue;
                                }
                            } // end both are arrays
                        }
                    }
                    // x. Eliminate a dependence if a dependence involving two different array references and no-aliasing is assumed.
                    SgExpression *src_array_exp = nullptr;
                    SgExpression *snk_array_exp = nullptr;
                    SgExpression *src_exp = isSgExpression(src_node);
                    SgExpression *snk_exp = isSgExpression(snk_node);
                    if (src_exp && snk_exp)
                    {
                        SageInterface::isArrayReference(src_exp, &src_array_exp);
                        SageInterface::isArrayReference(snk_exp, &snk_array_exp);

                        if (isArray1 && isArray2 && src_array_exp && snk_array_exp)
                        {

                            SgInitializedName *src_array_iname = SageInterface::convertRefToInitializedName(src_array_exp);
                            SgInitializedName *snk_array_iname = SageInterface::convertRefToInitializedName(snk_array_exp);

                            SgSymbol *src_sym = src_array_iname->search_for_symbol_from_symbol_table();
                            SgSymbol *snk_sym = snk_array_iname->search_for_symbol_from_symbol_table();
                            if (src_sym != snk_sym)
                            {
                                if (AP::Config::get().enable_debug)
                                    std::cout << "Both source and sink reference are array references..." << std::endl;

                                if ((info.GetDepType() & DEPTYPE_ANTI) || (info.GetDepType() & DEPTYPE_TRUE) || (info.GetDepType() & DEPTYPE_OUTPUT))
                                {
                                    if (AP::Config::get().enable_debug)
                                        std::cout << "\t Dep type is TRUE_DEP or ANTI_DEP or OUTPUT_DEP" << std::endl;
                                    if (AP::Config::get().no_aliasing)
                                    {
                                        if (AP::Config::get().enable_debug)
                                        {
                                            std::cout << "Non-aliasing assumed, eliminating a dep relation due to two pointers used as arrays)" << std::endl;
                                            info.Dump();
                                        }
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                    // x. Eliminate dependencies caused by autoscoped variables
                    //  -----------------------------------------------
                    //  such as private, firstprivate, lastprivate, and reduction
                    if (att && (src_name || snk_name)) // either src or snk might be an array reference
                    {
                        std::vector<SgInitializedName *> scoped_vars = CollectAllowedScopedVariables(att);
                        auto hit1 = scoped_vars.end();
                        auto hit2 = scoped_vars.end();
                        if (src_name)
                            hit1 = find(scoped_vars.begin(), scoped_vars.end(), src_name);
                        if (snk_name)
                            hit2 = find(scoped_vars.begin(), scoped_vars.end(), snk_name);
                        if (hit1 != scoped_vars.end() || (hit2 != scoped_vars.end()))
                        {
                            if (AP::Config::get().enable_debug)
                            {
                                std::cout << "Eliminating a dep relation due to at least one autoscoped variables" << std::endl;
                                info.Dump();
                            }
                            continue;
                        }
                    }

                    // x. Eliminate dependencies caused by a pair of indirect indexed array reference,
                    //  -----------------------------------------------
                    //    if users provide the semantics that all indirect indexed array references have
                    //    unique element accesses (via -rose:autopar:unique_indirect_index )
                    //    Since each iteration will access a unique element of the array, no loop carried data dependences
                    //   Lookup the table, rule out a dependence relationship if both source and sink are one of the unique array reference expressions.
                    //   AND both references to the same array symbol , and uses the same index variable!!
                    if (AP::Config::get().b_unique_indirect_index)
                    {
                        if (indirect_table[src_node] && indirect_table[snk_node])
                        {
                            if (AP::Config::get().enable_debug)
                            {
                                std::cout << "Eliminating a dep relation due to unique indirect indexed array references" << std::endl;
                                info.Dump();
                            }
                            continue;
                        }
                    }
                    // This is useful for since two data member accesses will point to the same variable symbol
                    // even when they are from different objects of the same class.
                    // The current dependence analysis will treat them as the same memory access
                    //
                    // We check objects are the same or not
                    // this can be useful for some input code
                    //
                    // Liao, 7/6/2016
                    // x. Eliminate dependencies between two different memory locations
                    // -----------------------------------------------

                    // SgExpression* src_exp = isSgExpression(src_node);
                    // SgExpression* snk_exp = isSgExpression(snk_node);
                    if (src_exp && snk_exp)
                    {
                        if (differentMemoryLocation(src_exp, snk_exp))
                        {
                            if (AP::Config::get().enable_debug)
                            {
                                std::cout << "Eliminating a dep relation between two instances of the same data member from different parent aggregate data" << std::endl;
                                info.Dump();
                            }
                            continue;
                        }
                    }
                    // x. Eliminate dependencies  without common enclosing loop nests
                    // -----------------------------------------------
                    if (info.CommonLevel() == 0)
                    {
                        if (AP::Config::get().enable_debug)
                        {
                            std::cout << "Eliminating a dep relation due to lack of common enclosing loop nests: common level ==0" << std::endl;
                            info.Dump();
                        }
                        continue;
                    }

                    // x. Eliminate loop-independent dependencies:
                    // -----------------------------------------------
                    // loop independent dependencies: privatization can eliminate most of them
                    if (info.CarryLevel() != 0)
                    {
                        if (AP::Config::get().enable_debug)
                        {
                            std::cout << "Eliminating a dep relation due to carryLevel != 0 (not carried by current loop level in question)" << std::endl;
                            info.Dump();
                        }
                        continue;
                    }
                    // Save the rest dependences which can not be ruled out
                    if (AP::Config::get().enable_debug)
                        std::cout << "\t this dep relation cannot be eliminated. saved into remaining depedence set." << std::endl;
                    remainings.push_back(info);
                } // end iterator edges for a node
            }     // end if has edge
        }         // end of iterate dependence graph

        if (AP::Config::get().enable_debug)
        {
            std::cout << "Exiting DependenceElimination ()" << std::endl;
        }

    } // end DependenceElimination()

    /*
     Uniforming multiple forms of array indirect accessing into a single form:
            arrayX[arrayY...[loop_index]]

      Common forms of array references using indirect indexing:

      Form 1:  naive one (the normalized/uniformed form)
            ... array_X [array_Y [current_loop_index]] ..

      Form 2:  used more often in real code from Jeff
           indirect_loop_index  = array_Y [current_loop_index] ;
            ...  array_X[indirect_loop_index] ...

      Cases of multiple dimensions, multiple levels of indirections are also handled.

      We uniform them into a single form (Form 1) to simplify later recognition of indirect indexed array refs

       For Form 2: if the rhs operand is a variable
          find the reaching definition of the index based on data flow analysis
            case 1: if it is the current loop's index variable, nothing to do further. stop
                    what if it is another loop's index variable??
            case 2: outside the current loop's scope: for example higher level loop's index. stop
            case 3: the definition is within the  current loop ?
                     replace the rhs operand with its reaching definition's right hand value.
                        if rhs is another array references??
                    one assignment to another array with the current  (?? higher level is considered at its own level) loop index

    Algorithm: Replace the index variable with its right hand value of its reaching definition,
               or if the definition 's scope is within the current  loop's body

    */
    static void uniformIndirectIndexedArrayRefs(SgForStatement *for_loop)
    {
        if (AP::Config::get().enable_debug)
            std::cout << "Entering uniformIndirectIndexedArrayRefs() ..." << std::endl;
        ROSE_ASSERT(for_loop != nullptr);
        ROSE_ASSERT(for_loop->get_loop_body() != nullptr);
        SgInitializedName *loop_index_name = nullptr;
        bool isCanonical = SageInterface::isCanonicalForLoop(for_loop, &loop_index_name);
        ROSE_ASSERT(isCanonical == true);

        // prepare def/use analysis, it should already exist as part of initialize_analysis()
        ROSE_ASSERT(l_defuse != nullptr);

        // For each array reference:
        std::vector<SgPntrArrRefExp *> nodeList =
            SageInterface::querySubTree<SgPntrArrRefExp>(for_loop->get_loop_body(), V_SgPntrArrRefExp);
        for (SgPntrArrRefExp *aRef : nodeList)
        {
            SgExpression *rhs = aRef->get_rhs_operand_i();
            switch (rhs->variantT())
            {
            case V_SgVarRefExp: // the index of the array is a variable reference
            {
                // SgVarRefExp * varRef = isSgVarRefExp(rhs);
                // trace back to the 'root' value of rhs according to def/use analysis
                // Initialize the end value to the current rhs of the array reference expression
                SgExpression *the_end_value = rhs;
                while (isSgVarRefExp(the_end_value)) // the applications we care only have one level of value transfer.
                {
                    SgVarRefExp *varRef = isSgVarRefExp(the_end_value);
                    SgInitializedName *initName = isSgInitializedName(varRef->get_symbol()->get_declaration());
                    ROSE_ASSERT(initName != nullptr);
                    // stop tracing if it is already the current loop's index
                    if (initName == loop_index_name)
                        break;

                    // get the reaching definitions of the variable
                    std::vector<SgNode *> vec = l_defuse->getDefFor(varRef, initName);
                    if (vec.size() == 0)
                    {
                        std::cerr << "Warning: cannot find a reaching definition for an initialized name:" << std::endl;
                        std::cerr << "initName:" << initName->get_name().getString() << "@";
                        std::cerr << varRef->get_file_info()->get_line() << ":" << varRef->get_file_info()->get_col() << std::endl;
                        // ROSE_ASSERT (vec.size()>0);
                        break;
                    }

                    // stop tracing if there are more than one reaching definitions
                    if (vec.size() > 1)
                        break;

                    // stop if the defining statement is out side of the scope of the loop body
                    SgStatement *def_stmt = SageInterface::getEnclosingStatement(vec[0]);
                    if (!SageInterface::isAncestor(for_loop->get_loop_body(), def_stmt))
                        break;

                    // now get the end value depending on the definition node's type
                    if (isSgAssignOp(vec[0]))
                        the_end_value = isSgAssignOp(vec[0])->get_rhs_operand_i();
                    else if (isSgAssignInitializer(vec[0]))
                    {
                        the_end_value = isSgAssignInitializer(vec[0])->get_operand_i();
                    }
                    else
                    {
                        if (!isSgMinusMinusOp(vec[0])) // (! && !)
                        {
                            std::cerr << "Warning: uniformIndirectIndexedArrayRefs() ignoring a reaching definition of a type: "
                                      << vec[0]->class_name() << "@";
                            if (isSgLocatedNode(vec[0]))
                            {
                                SgLocatedNode *lnode = isSgLocatedNode(vec[0]);
                                std::cerr << lnode->get_file_info()->get_line() << ":" << lnode->get_file_info()->get_col();
                            }
                            std::cerr << std::endl;
                        }
                        // ROSE_ASSERT(false);
                        break;
                    }
                } // end while() to trace down to root definition expression

                // Replace rhs with its root value if rhs != end_value
                //  We should only do the replacement if the end value is array reference!
                //  Otherwise, an inner loop variable j's initialization value 0 will be used to replace j in the array reference!
                //  Liao, 12/20/2017
                if (isSgPntrArrRefExp(the_end_value) && rhs != the_end_value)
                {
                    SgExpression *new_rhs = SageInterface::deepCopy<SgExpression>(the_end_value);
                    // TODO use replaceExpression() instead
                    aRef->set_rhs_operand_i(new_rhs);
                    new_rhs->set_parent(aRef);
                    delete rhs;
                }

                break;
            }                       // end case V_SgVarRefExp:
            case V_SgPntrArrRefExp: // uniform form already, do nothing
            case V_SgIntVal:        // element access using number, do nothing
                                    // ignore array index arithmetics
                                    // since we narrow down the simplest case for indirection without additional calculation
            case V_SgSubtractOp:
            case V_SgAddOp:
            case V_SgMinusMinusOp:
            case V_SgPlusPlusOp:
            case V_SgModOp:
            case V_SgMultiplyOp:
                break;
            default:
            {
                std::cerr << "Warning: uniformIndirectIndexedArrayRefs(): ignoring an array access expression type: " << rhs->class_name() << std::endl;
                break;
            }
            } // end switch
        }     // end for
    }
    /* Check if an array reference expression is an indirect indexed with respect to a loop
     * This function should be called after all array references are uniformed already.
     *
    Algorithm:
      find all array variables within the loop in consideration
      for each array variable, do the following to tell if such an array is accessed via an indirect index
          SgPntrArrRefExp  :
              lhs_operatnd: SgVarRefExp, SgVariableSymbol, SgInitializedName  SgArrayType
              rhs_operand: SgVarRefExp, SgVariableSymbol, SgInitializedName, i
          Check a lookup table to see if this kind of reference is already recognized
                two keys: array symbol, index expression, bool
          if not, do the actual pattern recognition
              // in a function
              if is another array reference
              Found an array reference using indirect index,
           store it in a look up table : SySymbol (array being accessed ) , index Ref Exp, true/false
     */

    static bool isIndirectIndexedArrayRef(SgForStatement *for_loop, SgPntrArrRefExp *aRef)
    {
        bool rtval = false;
        ROSE_ASSERT(for_loop != nullptr);
        ROSE_ASSERT(aRef != nullptr);
        // grab the loop index variable
        SgInitializedName *loop_index_name = nullptr;
        bool isCanonical = SageInterface::isCanonicalForLoop(for_loop, &loop_index_name);
        bool hasIndirecting = false;
        ROSE_ASSERT(isCanonical == true);

        // grab the array index  from arrayX[arrayY...[loop_index]]
        SgPntrArrRefExp *innerMostArrExp = aRef;
        while (isSgPntrArrRefExp(innerMostArrExp->get_rhs_operand_i()))
        {
            innerMostArrExp = isSgPntrArrRefExp(innerMostArrExp->get_rhs_operand_i());
            hasIndirecting = true;
        }

        SgExpression *array_index_exp = innerMostArrExp->get_rhs_operand_i();

        switch (array_index_exp->variantT())
        {
        case V_SgPntrArrRefExp:
        {
            std::cerr << "Error: isIndirectIndexedArrayRef(). inner most loop index should not be of an array type anymore! " << std::endl;
            ROSE_ASSERT(false);
            break;
        }
        case V_SgVarRefExp:
        {
            SgVarRefExp *varRef = isSgVarRefExp(array_index_exp);
            // We only concern about the indirection based on the current loop's loop index variable
            // since we consider all loop levels one by one
            if (hasIndirecting && (varRef->get_symbol()->get_declaration() == loop_index_name))
                rtval = true;
            break;
        }
        case V_SgIntVal:
            // ignore array index arithmetics
            // since we narrow down the simplest case for indirection without additional calculation
        case V_SgSubtractOp:
        case V_SgAddOp:
        case V_SgPlusPlusOp:
        case V_SgMultiplyOp:
            break;
        default:
            // This should not matter. We output something anyway for improvements.
            std::cerr << "Warning: isIndirectIndexedArrayRef(): unhandled array index type: " << array_index_exp->class_name() << std::endl;
            //  ROSE_ASSERT (false);
            break;
        }

        return rtval;
    }

    // collect array references with indirect indexing within a loop, save the result in a lookup table
    /*
    Algorithm:
      find all array variables within the loop in consideration
      for each array variable, do the following to tell if such an array is accessed via an indirect index
         Check a lookup table to see if this kind of reference is already recognized
                two keys: array symbol, index expression, bool
          if not, do the actual pattern recognition
             Found an array reference using indirect index,
           store it in a look up table : SySymbol (array being accessed ) , index Ref Exp, true/false
     */
    static void collectIndirectIndexedArrayReferences(SgNode *loop, std::map<SgNode *, bool> &indirect_array_table)
    {
        ROSE_ASSERT(loop != nullptr);
        SgForStatement *for_loop = isSgForStatement(loop);
        ROSE_ASSERT(for_loop != nullptr);

        std::vector<SgPntrArrRefExp *> nodeList =
            SageInterface::querySubTree<SgPntrArrRefExp>(for_loop->get_loop_body(), V_SgPntrArrRefExp);
        for (SgPntrArrRefExp *aRef : nodeList)
        {
            if (isIndirectIndexedArrayRef(for_loop, aRef))
            {
                indirect_array_table[aRef] = true;
                // std::cout<<"Found an indirect indexed array ref:"<<aRef->unparseToString()
                // << "@" << aRef <<std::endl;
            }
        }
    }

    bool CanParallelizeOutermostLoop(SgNode *loop, ArrayInterface *array_interface, ArrayAnnotation *annot)
    {
        ROSE_ASSERT(loop && array_interface && annot);
        ROSE_ASSERT(isSgForStatement(loop));
        bool isParallelizable = true;

        int dep_dist = 999999; // the minimum dependence distance of all dependence relations for a loop.

        // collect array references with indirect indexing within a loop, save the result in a lookup table
        // This work is context sensitive (depending on the outer loops), so we declare the table for each loop.
        std::map<SgNode *, bool> indirect_array_table;
        if (AP::Config::get().b_unique_indirect_index) // uniform and collect indirect indexed array only when needed
        {
            // uniform array reference expressions
            uniformIndirectIndexedArrayRefs(isSgForStatement(loop));
            collectIndirectIndexedArrayReferences(loop, indirect_array_table);
        }

        SgNode *sg_node = loop;
        std::string filename = sg_node->get_file_info()->get_filename();
        int lineno = sg_node->get_file_info()->get_line();
        int colno = sg_node->get_file_info()->get_col();

        // X. Compute dependence graph for the target loop
        LoopTreeDepGraph *depgraph = ComputeDependenceGraph(sg_node, array_interface, annot);
        if (depgraph == nullptr)
        {
            std::cout << "Warning: skipping a loop at line " << lineno << " since failed to compute depgraph for it:";
            //<<sg_node->unparseToString()<<std::endl;
            return false;
        }

        // X. Variable classification (autoscoping):
        // This step is done before DependenceElimination(), so the irrelevant
        // dependencies associated with the autoscoped variables can be
        // eliminated.
        std::unique_ptr<OmpSupport::OmpAttribute> omp_attribute(buildOmpAttribute(OmpSupport::e_unknown, nullptr, false));
        ROSE_ASSERT(omp_attribute != nullptr);

        AutoScoping(sg_node, omp_attribute.get(), depgraph);

        // If there are unallowed autoscoped, the loop is not parallelizable
        std::vector<SgInitializedName *> unallowed_scoped_variables = CollectUnallowedScopedVariables(omp_attribute.get());
        if (!unallowed_scoped_variables.empty())
        {
            isParallelizable = false;
            std::ostringstream oss;
            oss << "Unparallelizable loop@" << filename << ":" << lineno << ":" << colno << std::endl;

            if (AP::Config::get().enable_debug) // diff user vs. autopar needs cleaner output
            {
                std::cout << "=====================================================" << std::endl;
                std::cout << "Unparallelizable loop at line:" << sg_node->get_file_info()->get_line() << " due to scoped variables of unallowed types:" << std::endl;
                for (auto name : unallowed_scoped_variables)
                {
                    std::cout << "  " << name->get_qualified_name().getString() << std::endl;
                }
            }
        }
        else
        {
            // X. Eliminate irrelevant dependence relations.
            std::vector<DepInfo> remainingDependences;
            DependenceElimination(sg_node, depgraph, remainingDependences, omp_attribute.get(), indirect_array_table, array_interface, annot);

            if (remainingDependences.size() > 0)
            {
                // write log entries for failed attempts
                isParallelizable = false;
                std::ostringstream oss;
                oss << "Unparallelizable loop@" << filename << ":" << lineno << ":" << colno << std::endl;

                if (AP::Config::get().enable_debug) // diff user vs. autopar needs cleaner output
                {
                    std::cout << "=====================================================" << std::endl;
                    std::cout << "Unparallelizable loop at line:" << sg_node->get_file_info()->get_line() << " due to the following dependencies:" << std::endl;
                    for (const auto &di : remainingDependences)
                    {
                        std::cout << di.toString() << std::endl;
                        if (di.rows() > 0 && di.cols() > 0)
                        {
                            int dist = abs((di.Entry(0, 0)).GetAlign());
                            if (dist < dep_dist)
                                dep_dist = dist;
                        }
                    }
                    std::cout << "The minimum dependence distance of all dependences for the loop is:" << dep_dist << std::endl;
                }
            }
        }

        // comp.DetachDepGraph();// TODO release resources here

        return isParallelizable;
    }

    // We maintain a blacklist of language features, put them into a set
    bool useUnsupportedLanguageFeatures(SgNode *loop, VariantT *blackConstruct)
    {
        std::set<VariantT> blackListDict;
        blackListDict.insert(V_SgRshiftOp);
        blackListDict.insert(V_SgLshiftOp);

        // build a dictionary of language constructs shown up in the loop, then query it
        RoseAst ast(loop);
        for (SgNode *node : ast)
        {
            if (blackListDict.find(node->variantT()) != blackListDict.end())
            {
                *blackConstruct = node->variantT();
                return true;
            }
        }
        return false;
    }

    // Not in use since we care about top level variables now
    // TODO: move to SageInterface later
    // strip off arrow, dot expressions and get down to smallest data member access expression
    SgExpression *getBottomVariableAccess(SgExpression *e)
    {
        SgExpression *ret = nullptr;
        ROSE_ASSERT(e != nullptr);
        if (isSgVarRefExp(e))
            ret = e;
        else if (SgDotExp *dot_exp = isSgDotExp(e))
        {
            ret = getBottomVariableAccess(dot_exp->get_rhs_operand());
        }
        else if (SgArrowExp *a_exp = isSgArrowExp(e))
        {
            ret = getBottomVariableAccess(a_exp->get_rhs_operand());
        }
        else if (SgPntrArrRefExp *arr_exp = isSgPntrArrRefExp(e))
        {
            ret = getBottomVariableAccess(arr_exp->get_lhs_operand_i());
        }

        if (ret == nullptr)
        {
            std::cerr << "getBottomVariableAccess() reached unhandled expression type:" << e->class_name() << std::endl;
            e->get_file_info()->display();
            ROSE_ASSERT(false);
        }

        return ret;
    }

    // For an expression, check if it is a data member of an aggregate data object (except array element access?)
    //  If so, return the parent aggregate data object's reference.
    //  This is done recursively when possible.
    //  TODO: move to SageInterface when ready
    //
    //  if already DotExp, return getTVA(lhs)
    //  if rhs, return getTVA()
    /*
    Nested structures

    mygun.mag.capacity

       SgDotExp
        /      \
     SgDotExp  capacity
       /   \
    mygun  mag
      * */
    SgExpression *getTopVariableAccess(SgExpression *e)
    {
        // default: self is the top already.
        SgExpression *ret = e;
        ROSE_ASSERT(e != nullptr);

        // check if it is a SgDotExp or ArrowExp first,
        // if So, walk to its left child
        if (SgDotExp *de = isSgDotExp(e))
        {
            ret = getTopVariableAccess(de->get_lhs_operand()); // recursive call to handle multiple levels
        }
        else if (SgArrowExp *ae = isSgArrowExp(e))
        {
            ret = getTopVariableAccess(ae->get_lhs_operand()); // recursive call to handle multiple levels
        }
        // otherwise, it could be either lhs or rhs of Dot or Arrow Exp
        else if (SgExpression *parent = isSgExpression(e->get_parent()))
        {
            if (SgDotExp *dot_exp = isSgDotExp(parent))
            {
                // a.b ?  call on DotExp
                if (dot_exp->get_rhs_operand() == e)
                    ret = getTopVariableAccess(dot_exp); // recursive call to handle multiple levels of aggregate data types
            }
            else if (SgArrowExp *a_exp = isSgArrowExp(parent))
            {
                // a-> b?  call on ArrowExp
                if (a_exp->get_rhs_operand() == e)
                    ret = getTopVariableAccess(a_exp);
            }
        }
        return ret;
    }

    // Obtain the underneath symbol from an expression, such as SgVarRefExp, SgThisExp, etc...
    // This function is used to find top level symbol
    // So when encountering dot or arrow expression, return the lhs symbol
    // TODO: move to SageInterface when ready
    SgSymbol *getSymbol(SgExpression *exp)
    {
        SgSymbol *s = nullptr;

        ROSE_ASSERT(exp != nullptr);

        if (SgVarRefExp *e = isSgVarRefExp(exp))
        {
            s = e->get_symbol();
        }
        else if (SgThisExp *e = isSgThisExp(exp))
            s = e->get_class_symbol();
        else if (SgPntrArrRefExp *e = isSgPntrArrRefExp(exp))
        {                                        // a[i]
            s = getSymbol(e->get_lhs_operand()); // recursive call here
        }
        else if (SgDotExp *e = isSgDotExp(exp))
        {
            s = getSymbol(e->get_lhs_operand()); // recursive call here
        }
        else if (SgArrowExp *e = isSgArrowExp(exp))
        {
            s = getSymbol(e->get_lhs_operand()); // recursive call here
        }
        else if (SgPointerDerefExp *e = isSgPointerDerefExp(exp))
        {
            s = getSymbol(e->get_operand_i()); // recursive call here
        }
        else if (SgAddOp *e = isSgAddOp(exp)) // * (address + offset) //TODO better handling here
        {
            s = getSymbol(e->get_lhs_operand()); // recursive call here
        }
        else if (SgCastExp *e = isSgCastExp(exp))
        {
            s = getSymbol(e->get_operand_i()); // recursive call here
        }
        else if (SgFunctionCallExp *e = isSgFunctionCallExp(exp))
            s = e->getAssociatedFunctionSymbol();
        else if (SgFunctionRefExp *e = isSgFunctionRefExp(exp))
            s = e->get_symbol_i();
        else if (SgMemberFunctionRefExp *e = isSgMemberFunctionRefExp(exp))
            s = e->get_symbol_i();
        else if (SgLabelRefExp *e = isSgLabelRefExp(exp))
            s = e->get_symbol();
        else if (isSgConstructorInitializer(exp)) // void reportAlgorithmStats(const std::string& err="");
        {                                         // temporary initializer on the right hand , assigned by value to left side, it has persistent no mem location is concerned.
            s = nullptr;
        }
        else
        {
            std::cerr << "Error. getSymbol(SgExpression* exp) encounters unhandled exp:" << exp->class_name() << std::endl;
            ROSE_ASSERT(false);
        }
        // We allow nullptr symbol here. Naturally eliminate some strange expressions in the dependence pair.
        //    ROSE_ASSERT (s!=nullptr);

        return s;
    }

    //! Check if two expressions access different memory locations from the same aggregate types.
    // If in double, return false (not certain, may alias to each other).
    // This is helpful to exclude some dependence relations involving two obvious different memory location accesses
    // TODO: move to SageInterface when ready
    // For example:  class VectorXY {int y} may have two different objects o1 and o2.
    // But o1.y and o2.y will be recognized as the same references to symbol y.
    // We need to get their parent objects and compare them.
    bool differentMemoryLocation(SgExpression *e1, SgExpression *e2)
    {
        bool retval = false;
        // if same expressions, not different then
        if (e1 == e2)
            return false;
        if ((e1 == nullptr) || (e2 == nullptr))
        {
            return false;
        }

        // now get down to the lowest levels
        SgExpression *var1 = getTopVariableAccess(e1);
        SgExpression *var2 = getTopVariableAccess(e2);

        // at this stage, dot or arrow expressions should be stripped off.
        ROSE_ASSERT(isSgDotExp(var1) == nullptr);
        ROSE_ASSERT(isSgArrowExp(var1) == nullptr);
        ROSE_ASSERT(isSgDotExp(var2) == nullptr);
        ROSE_ASSERT(isSgArrowExp(var2) == nullptr);

        if (var1 != nullptr && var2 != nullptr)
        { // We must check if e1's top variable is itself: If yes, no aggregate types are involved. e1 and e2 may be pointer scalars aliasing to each other
            if (getSymbol(var1) != getSymbol(var2) && (e1 != var1 && e2 != var2))
                retval = true; // pointing to two different parent symbols?
        }
        return retval;
    }
} // end namespace
