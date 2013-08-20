#include "subsumestrengthen.h"
#include "simplifier.h"
#include "solver.h"

using namespace CMSat;

SubsumeStrengthen::SubsumeStrengthen(
    Simplifier* _simplifier
    , Solver* _solver
) :
    simplifier(_simplifier)
    , solver(_solver)
{
}

/**
@brief Backward-subsumption using given clause

@p c The clause to use
@p cl The clause to use

*/
uint32_t SubsumeStrengthen::subsume0(ClOffset offset)
{
    Clause& cl = *solver->clAllocator->getPointer(offset);
    #ifdef VERBOSE_DEBUG
    cout << "subsume0-ing with clause: " << cl << endl;
    #endif

    Sub0Ret ret = subsume0AndUnlink(
        offset
        , cl
        , cl.abst
    );

    //If irred is subsumed by redundant, make the redundant into irred
    if (cl.red()
        && ret.subsumedNonRed
    ) {
        cl.makeNonRed();
        solver->binTri.redLits -= cl.size();
        solver->binTri.irredLits += cl.size();
        if (!cl.getOccurLinked()) {
            simplifier->linkInClause(cl);
        }
    }

    //Combine stats
    cl.combineStats(ret.stats);

    return ret.numSubsumed;
}
template SubsumeStrengthen::Sub0Ret SubsumeStrengthen::subsume0AndUnlink(
        const ClOffset offset
        , const vector<Lit>& ps
        , const CL_ABST_TYPE abs
        , const bool removeImplicit = false
    );

/**
@brief Backward-subsumption using given clause
*/
template<class T>
SubsumeStrengthen::Sub0Ret SubsumeStrengthen::subsume0AndUnlink(
    const ClOffset offset
    , const T& ps
    , const CL_ABST_TYPE abs
    , const bool removeImplicit
) {
    Sub0Ret ret;

    subs.clear();
    findSubsumed0(offset, ps, abs, subs, removeImplicit);

    //Go through each clause that can be subsumed
    for (vector<ClOffset>::const_iterator
        it = subs.begin(), end = subs.end()
        ; it != end
        ; it++
    ) {
        Clause *tmp = solver->clAllocator->getPointer(*it);
        #ifdef VERBOSE_DEBUG
        cout << "-> subsume0 removing:" << *tmp << endl;
        #endif

        //Combine stats
        ret.stats = ClauseStats::combineStats(tmp->stats, ret.stats);

        //At least one is irred. Indicate this to caller.
        if (!tmp->red())
            ret.subsumedNonRed = true;

        /*cout
        << "This " << ps << " (offset: " << offset << ") subsumed this: "
        << *tmp << "(offset: " << *it << ")"
        << endl;*/

        simplifier->unlinkClause(*it);
        ret.numSubsumed++;

        //If we are waaay over time, just exit
        if (*simplifier->toDecrease < -20LL*1000LL*1000LL)
            break;
    }

    return ret;
}

/**
@brief Backward subsumption and self-subsuming resolution

Performs backward subsumption AND
self-subsuming resolution using backward-subsumption
*/
SubsumeStrengthen::Sub1Ret SubsumeStrengthen::subsume1(const ClOffset offset)
{
    subs.clear();
    subsLits.clear();
    Sub1Ret ret;
    Clause& cl = *solver->clAllocator->getPointer(offset);

    if (solver->conf.verbosity >= 6)
        cout << "subsume1-ing with clause:" << cl << endl;

    findStrengthened(
        offset
        , cl
        , cl.abst
        , subs
        , subsLits
    );

    for (size_t j = 0
        ; j < subs.size() && solver->okay()
        ; j++
    ) {
        ClOffset offset2 = subs[j];
        Clause& cl2 = *solver->clAllocator->getPointer(offset2);
        if (subsLits[j] == lit_Undef) {  //Subsume

            if (solver->conf.verbosity >= 6)
                cout << "subsumed clause " << cl2 << endl;

            //If subsumes a irred, and is redundant, make it irred
            if (cl.red()
                && !cl2.red()
            ) {
                cl.makeNonRed();
                solver->binTri.redLits -= cl.size();
                solver->binTri.irredLits += cl.size();
                if (!cl.getOccurLinked()) {
                    simplifier->linkInClause(cl);
                }
            }

            //Update stats
            cl.combineStats(cl2.stats);

            simplifier->unlinkClause(offset2);
            ret.sub++;
        } else { //Strengthen
            if (solver->conf.verbosity >= 6) {
                cout << "strenghtened clause " << cl2 << endl;
            }
            strengthen(offset2, subsLits[j]);

            ret.str++;
            if (!solver->ok)
                return ret;

            //If we are waaay over time, just exit
            if (*simplifier->toDecrease < -20LL*1000LL*1000LL)
                break;
        }
    }

    return ret;
}

void SubsumeStrengthen::performSubsumption()
{
    //If clauses are empty, the system below segfaults
    if (simplifier->clauses.empty())
        return;

    double myTime = cpuTime();
    size_t wenThrough = 0;
    size_t subsumed = 0;
    simplifier->toDecrease = &simplifier->numMaxSubsume0;
    while (*simplifier->toDecrease > 0
        && wenThrough < 1.5*(double)simplifier->clauses.size()
    ) {
        *simplifier->toDecrease -= 2;

        //Print status
        if (solver->conf.verbosity >= 5
            && wenThrough % 10000 == 0
        ) {
            cout << "toDecrease: " << *simplifier->toDecrease << endl;
        }

        const size_t num = solver->mtrand.randInt(simplifier->clauses.size()-1);
        ClOffset offset = simplifier->clauses[num];
        Clause* cl = solver->clAllocator->getPointer(offset);

        //Has already been removed
        if (cl->getFreed())
            continue;

        wenThrough++;
        *simplifier->toDecrease -= 20;

        subsumed += subsume0(offset);
    }

    if (solver->conf.verbosity >= 3) {
        cout
        << "c subs: " << subsumed
        << " tried: " << wenThrough
        << " T: " << cpuTime() - myTime
        << endl;
    }

    //Update time used
    runStats.subsumedBySub += subsumed;
    runStats.subsumeTime += cpuTime() - myTime;
}

bool SubsumeStrengthen::performStrengthening()
{
    assert(solver->ok);

    double myTime = cpuTime();
    size_t wenThrough = 0;
    simplifier->toDecrease = &simplifier->numMaxSubsume1;
    Sub1Ret ret;
    while(*simplifier->toDecrease > 0
        && wenThrough < 1.5*(double)2*simplifier->clauses.size()
        && solver->okay()
    ) {
        *simplifier->toDecrease -= 20;
        wenThrough++;

        //Print status
        if (solver->conf.verbosity >= 5
            && wenThrough % 10000 == 0
        ) {
            cout << "toDecrease: " << *simplifier->toDecrease << endl;
        }

        size_t num = solver->mtrand.randInt(simplifier->clauses.size()-1);
        ClOffset offset = simplifier->clauses[num];
        Clause* cl = solver->clAllocator->getPointer(offset);

        //Has already been removed
        if (cl->getFreed())
            continue;

        ret += subsume1(offset);

    }

    if (solver->conf.verbosity >= 3) {
        cout
        << "c streng sub: " << ret.sub
        << " str: " << ret.str
        << " tried: " << wenThrough
        << " T: " << cpuTime() - myTime
        << endl;
    }

    //Update time used
    runStats.subsumedByStr += ret.sub;
    runStats.litsRemStrengthen += ret.str;
    runStats.strengthenTime += cpuTime() - myTime;

    return solver->ok;
}

/**
@brief Helper function for findStrengthened

Used to avoid duplication of code
*/
template<class T>
void inline SubsumeStrengthen::fillSubs(
    const ClOffset offset
    , const T& cl
    , const CL_ABST_TYPE abs
    , vector<ClOffset>& out_subsumed
    , vector<Lit>& out_lits
    , const Lit lit
) {
    Lit litSub;
    const vec<Watched>& cs = solver->watches[lit.toInt()];
    *simplifier->toDecrease -= cs.size()*15 + 40;
    for (vec<Watched>::const_iterator
        it = cs.begin(), end = cs.end()
        ; it != end
        ; it++
    ) {
        if (!it->isClause())
            continue;

        if (it->getOffset() == offset
            || !subsetAbst(abs, it->getAbst())
        ) {
            continue;
        }

        ClOffset offset2 = it->getOffset();
        const Clause& cl2 = *solver->clAllocator->getPointer(offset2);

        if (cl.size() > cl2.size())
            continue;

        *simplifier->toDecrease -= cl.size() + cl2.size();
        litSub = subset1(cl, cl2);
        if (litSub != lit_Error) {
            out_subsumed.push_back(it->getOffset());
            out_lits.push_back(litSub);

            #ifdef VERBOSE_DEBUG
            if (litSub == lit_Undef) cout << "subsume0-d: ";
            else cout << "subsume1-ed (lit: "
                << litSub
                << ") clause offset: "
                << it->getOffset()
                << endl;
            #endif
        }
    }
}

/**
@brief Checks if clauses are subsumed or could be strenghtened with given clause

Checks if:
* any clause is subsumed with given clause
* the given clause could perform self-subsuming resolution on any other clause

@param[in] ps The clause to perform the above listed algos with
@param[in] abs The abstraction of clause ps
@param[out] out_subsumed The clauses that could be modified by ps
@param[out] out_lits Defines HOW these clauses could be modified. By removing
literal, or by subsumption (in this case, there is lit_Undef here)
*/
template<class T>
void SubsumeStrengthen::findStrengthened(
    ClOffset offset
    , const T& cl
    , const CL_ABST_TYPE abs
    , vector<ClOffset>& out_subsumed
    , vector<Lit>& out_lits
)
{
    #ifdef VERBOSE_DEBUG
    cout << "findStrengthened: " << cl << endl;
    #endif

    Var minVar = var_Undef;
    uint32_t bestSize = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < cl.size(); i++){
        uint32_t newSize =
            solver->watches[cl[i].toInt()].size()
                + solver->watches[(~cl[i]).toInt()].size();

        if (newSize < bestSize) {
            minVar = cl[i].var();
            bestSize = newSize;
        }
    }
    assert(minVar != var_Undef);
    *simplifier->toDecrease -= cl.size();

    fillSubs(offset, cl, abs, out_subsumed, out_lits, Lit(minVar, true));
    fillSubs(offset, cl, abs, out_subsumed, out_lits, Lit(minVar, false));
}

void SubsumeStrengthen::strengthen(ClOffset offset, const Lit toRemoveLit)
{
    Clause& cl = *solver->clAllocator->getPointer(offset);
    #ifdef VERBOSE_DEBUG
    cout << "-> Strenghtening clause :" << cl;
    cout << " with lit: " << toRemoveLit << endl;
    #endif

    *simplifier->toDecrease -= 5;
    #ifdef DRUP
    vector<Lit> origCl(cl.size());
    std::copy(cl.begin(), cl.end(), origCl.begin());
    #endif
    cl.strengthen(toRemoveLit);
    #ifdef DRUP
    if (solver->drup) {
        *(solver->drup)
        << cl
        << " 0\n"
        << "d " << origCl
        << " 0\n";
    }
    #endif

    runStats.litsRemStrengthen++;
    removeWCl(solver->watches[toRemoveLit.toInt()], offset);
    if (cl.red())
        solver->binTri.redLits--;
    else
        solver->binTri.irredLits--;

    simplifier->cleanClause(offset);
}
/**
@brief Decides only using abstraction if clause A could subsume clause B

@note: It can give false positives. Never gives false negatives.

For A to subsume B, everything that is in A MUST be in B. So, if (A & ~B)
contains even one bit, it means that A contains something that B doesn't. So
A may be a subset of B only if (A & ~B) == 0
*/
bool SubsumeStrengthen::subsetAbst(const CL_ABST_TYPE A, const CL_ABST_TYPE B)
{
    return ((A & ~B) == 0);
}

//A subsumes B (A <= B)
template<class T1, class T2>
bool SubsumeStrengthen::subset(const T1& A, const T2& B)
{
    #ifdef MORE_DEUBUG
    cout << "A:" << A << endl;
    for(size_t i = 1; i < A.size(); i++) {
        assert(A[i-1] < A[i]);
    }

    cout << "B:" << B << endl;
    for(size_t i = 1; i < B.size(); i++) {
        assert(B[i-1] < B[i]);
    }
    #endif

    bool ret;
    uint16_t i = 0;
    uint16_t i2;
    Lit lastB = lit_Undef;
    for (i2 = 0; i2 != B.size(); i2++) {
        if (lastB != lit_Undef)
            assert(lastB < B[i2]);

        lastB = B[i2];
        //Literals are ordered
        if (A[i] < B[i2]) {
            ret = false;
            goto end;
        }
        else if (A[i] == B[i2]) {
            i++;

            //went through the whole of A now, so A subsumes B
            if (i == A.size()) {
                ret = true;
                goto end;
            }
        }
    }
    ret = false;

    end:
    *simplifier->toDecrease -= i2*4 + i*4;
    return ret;
}

/**
@brief Decides if A subsumes B, or if not, if A could strenghten B

@note: Assumes 'seen' is cleared (will leave it cleared)

Helper function findSubsumed1. Does two things in one go:
1) decides if clause A could subsume clause B
2) decides if clause A could be used to perform self-subsuming resoltuion on
clause B

@return lit_Error, if neither (1) or (2) is true. Returns lit_Undef (1) is true,
and returns the literal to remove if (2) is true
*/
template<class T1, class T2>
Lit SubsumeStrengthen::subset1(const T1& A, const T2& B)
{
    Lit retLit = lit_Undef;

    uint16_t i = 0;
    uint16_t i2;
    for (i2 = 0; i2 != B.size(); i2++) {
        if (A[i] == ~B[i2] && retLit == lit_Undef) {
            retLit = B[i2];
            i++;
            if (i == A.size())
                goto end;

            continue;
        }

        //Literals are ordered
        if (A[i] < B[i2]) {
            retLit = lit_Error;
            goto end;
        }

        if (A[i] == B[i2]) {
            i++;

            if (i == A.size())
                goto end;
        }
    }
    retLit = lit_Error;

    end:
    *simplifier->toDecrease -= i2*4 + i*4;
    return retLit;
}

/**
@brief Finds clauses that are backward-subsumed by given clause

Only handles backward-subsumption. Uses occurrence lists
@param[out] out_subsumed The set of clauses subsumed by the given
*/
template<class T> void SubsumeStrengthen::findSubsumed0(
    const ClOffset offset //Will not match with index of the name value
    , const T& ps //Literals in clause
    , const CL_ABST_TYPE abs //Abstraction of literals in clause
    , vector<ClOffset>& out_subsumed //List of clause indexes subsumed
    , bool removeImplicit
) {
    #ifdef VERBOSE_DEBUG
    cout << "findSubsumed0: ";
    for (uint32_t i = 0; i < ps.size(); i++) {
        cout << ps[i] << " , ";
    }
    cout << endl;
    #endif

    //Which literal in the clause has the smallest occur list? -- that will be picked to go through
    size_t min_i = 0;
    for (uint32_t i = 1; i < ps.size(); i++){
        if (solver->watches[ps[i].toInt()].size() < solver->watches[ps[min_i].toInt()].size())
            min_i = i;
    }
    *simplifier->toDecrease -= ps.size();

    //Go through the occur list of the literal that has the smallest occur list
    vec<Watched>& occ = solver->watches[ps[min_i].toInt()];
    *simplifier->toDecrease -= occ.size()*8 + 40;

    vec<Watched>::iterator it = occ.begin();
    vec<Watched>::iterator it2 = occ.begin();
    size_t numBinFound = 0;
    for (vec<Watched>::const_iterator
        end = occ.end()
        ; it != end
        ; it++
    ) {
        if (removeImplicit) {
            if (it->isBinary()
                && ps.size() == 2
                && ps[!min_i] == it->lit2()
                && !it->red()
            ) {
                /*cout
                << "ps " << ps << " could subsume this bin: "
                << ps[min_i] << ", " << it->lit2()
                << endl;*/
                numBinFound++;

                //We cannot remove ourselves
                if (numBinFound > 1) {
                    removeWBin(solver->watches, it->lit2(), ps[min_i], it->red());
                    solver->binTri.irredBins--;
                    continue;
                }
            }

            if (it->isTri()
                && ps.size() == 2
                && (ps[!min_i] == it->lit2() || ps[!min_i] == it->lit3())
            ) {
                /*cout
                << "ps " << ps << " could subsume this tri: "
                << ps[min_i] << ", " << it->lit2() << ", " << it->lit3()
                << endl;
                */
                Lit lits[3];
                lits[0] = ps[min_i];
                lits[1] = it->lit2();
                lits[2] = it->lit3();
                std::sort(lits + 0, lits + 3);
                removeTriAllButOne(solver->watches, ps[min_i], lits, it->red());
                if (it->red()) {
                    solver->binTri.redTris--;
                } else {
                    solver->binTri.irredTris--;
                }
                continue;
            }
        }
        *it2++ = *it;

        if (!it->isClause()) {
            continue;
        }

        *simplifier->toDecrease -= 15;

        if (it->getOffset() == offset
            || !subsetAbst(abs, it->getAbst())
        ) {
            continue;
        }

        ClOffset offset2 = it->getOffset();
        const Clause& cl2 = *solver->clAllocator->getPointer(offset2);

        if (ps.size() > cl2.size())
            continue;

        *simplifier->toDecrease -= 50;
        if (subset(ps, cl2)) {
            out_subsumed.push_back(it->getOffset());
            #ifdef VERBOSE_DEBUG
            cout << "subsumed cl offset: " << it->getOffset() << endl;
            #endif
        }
    }
    occ.shrink(it-it2);
}
template void SubsumeStrengthen::findSubsumed0(
    const ClOffset offset
    , const boost::array<Lit, 2>& ps
    , const CL_ABST_TYPE abs //Abstraction of literals in clause
    , vector<ClOffset>& out_subsumed //List of clause indexes subsumed
    , bool removeImplicit
);

uint64_t SubsumeStrengthen::memUsed() const
{
    uint64_t b = 0;
    b += subs.capacity()*sizeof(ClOffset);
    b += subsLits.capacity()*sizeof(Lit);

    return b;
}

void SubsumeStrengthen::finishedRun()
{
    globalstats += runStats;
}


// struct WatchTriFirst
// {
//     bool operator()(const Watched& a, const Watched& b)
//     {
//         WatchType aType = a.getType();
//         WatchType bType = b.getType();
//
//         //Equal? Undecidable
//         if (aType == bType)
//             return false;
//
//         //One is binary, but the other isn't? Return that
//         if (aType == watch_binary_t)
//             return true;
//         if (bType == watch_binary_t)
//             return false;
//
//         //At this point neither is binary, and they are unequal
//
//         //One is tri, but the other isn't? Return that
//         if (aType == watch_tertiary_t)
//             return true;
//         if (bType == watch_tertiary_t)
//             return false;
//
//         //At this point, both must be clause, but that's impossible
//         assert(false);
//     }
// };

// bool Simplifier::subsumeWithTris()
// {
//     vector<Lit> lits;
//     size_t strSucceed = 0;
//
//     //Stats
//     toDecrease = &numMaxTriSub;
//     const size_t origTrailSize = solver->trail.size();
//     double myTime = cpuTime();
//     size_t subsumed = 0;
//
//     //Randomize start in the watchlist
//     size_t upI;
//     upI = solver->mtrand.randInt(solver->watches.size()-1);
//
//     size_t tried = 0;
//     size_t numDone = 0;
//     for (; numDone < solver->watches.size() && *simplifier->toDecrease > 0
//         ; upI = (upI +1) % solver->watches.size(), numDone++
//
//     ) {
//         Lit lit = Lit::toLit(upI);
//         vec<Watched>& ws = solver->watches[upI];
//
//         //Must re-order so that TRI-s are first
//         //Otherwise we might re-order list while looking through.. very messy
//         WatchTriFirst sorter;
//         std::sort(ws.begin(), ws.end(), sorter);
//
//         for (size_t i = 0
//             ; i < ws.size() && *simplifier->toDecrease > 0
//             ; i++
//         ) {
//             //Each TRI only once
//             if (ws[i].isTri()
//                 && lit < ws[i].lit2()
//                 && ws[i].lit2() < ws[i].lit3()
//             ) {
//                 tried++;
//                 lits.resize(3);
//                 lits[0] = lit;
//                 lits[1] = ws[i].lit2();
//                 lits[2] = ws[i].lit3();
//                 CL_ABST_TYPE abstr = calcAbstraction(lits);
//
//                 Sub0Ret ret = subsume0Final(
//                     std::numeric_limits<ClOffset>::max()
//                     , lits
//                     , abstr
//                 );
//
//                 subsumed += ret.numSubsumed;
//
//                 if (ws[i].red()
//                     && ret.subsumedNonRed
//                 ) {
//                     ws[i].setRed(false);
//                     solver->binTri.redTris--;
//                     solver->binTri.irredTris++;
//                     findWatchedOfTri(solver->watches, ws[i].lit2(), lit, ws[i].lit3(), true).setRed(false);
//                     findWatchedOfTri(solver->watches, ws[i].lit3(), lit, ws[i].lit2(), true).setRed(false);
//                 }
//             }
//         }
//
//         if (!solver->okay())
//             break;
//     }
//
//     if (solver->conf.verbosity >= 2) {
//         cout
//         << "c [subs] tri"
//         << " subs: " << subsumed
//         << " tried: " << tried
//         << " str: " << strSucceed
//         << " toDecrease: " << *simplifier->toDecrease
//         << " 0-depth ass: " << solver->trail.size() - origTrailSize
//         << " time: " << cpuTime() - myTime
//         << endl;
//     }
//
//     //runStats.zeroDepthAssigns = solver->trail.size() - origTrailSize;
//
//     return solver->ok;
// }

