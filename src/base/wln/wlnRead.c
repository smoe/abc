/**CFile****************************************************************

  FileName    [wln.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Word-level network.]

  Synopsis    []

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - September 23, 2018.]

  Revision    [$Id: wln.c,v 1.00 2018/09/23 00:00:00 alanmi Exp $]

***********************************************************************/

#include "wln.h"
#include "proof/cec/cec.h"

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

#define MAX_LINE 10000

#define MAX_MAP     32
#define CELL_NUM     8
#define WIRE_NUM     5
#define TEMP_NUM     5

//typedef struct Rtl_Lib_t_  Rtl_Lib_t;
struct Rtl_Lib_t_ 
{
    char *                pSpec;     // input file name
    Vec_Ptr_t *           vNtks;     // modules
    Abc_Nam_t *           pManName;  // object names
    Vec_Int_t             vConsts;   // constants
    Vec_Int_t             vSlices;   // selections
    Vec_Int_t             vConcats;  // concatenations
    FILE *                pFile;     // temp file
    Vec_Int_t *           vTokens;   // temp tokens
    int                   pMap[MAX_MAP];  // temp map
    Vec_Int_t *           vMap;      // mapping NameId into wires
    Vec_Int_t             vAttrTemp; // temp
    Vec_Int_t             vTemp[TEMP_NUM];  // temp
};

typedef struct Rtl_Ntk_t_  Rtl_Ntk_t;
struct Rtl_Ntk_t_ 
{
    int                   NameId;    // model name
    int                   nInputs;   // word-level inputs
    int                   nOutputs;  // word-level outputs
    Vec_Int_t             vWires;    // wires (name{upto,signed,in,out}+width+offset+number)
    Vec_Int_t             vCells;    // instances ([0]type+[1]name+[2]mod+[3]ins+[4]nattr+[5]nparams+[6]nconns+[6]mark+(attr+params+conns))
    Vec_Int_t             vConns;    // connection pairs
    Vec_Int_t             vStore;    // storage for cells
    Vec_Int_t             vAttrs;    // attributes
    Rtl_Lib_t *           pLib;      // parent
    Vec_Int_t             vOrder;    // topological order
    Vec_Int_t             vLits;     // bit-level view
    Vec_Int_t             vBitTemp;  // storage for bits
    Gia_Man_t *           pGia;      // derived by bit-blasting
    int                   Slice0;    // first slice
    int                   Slice1;    // last slice
    int                   iCopy;     // place in array
};

static inline int         Rtl_LibNtkNum( Rtl_Lib_t * pLib )                { return Vec_PtrSize(pLib->vNtks);                  }
static inline Rtl_Ntk_t * Rtl_LibNtk( Rtl_Lib_t * pLib, int i )            { return (Rtl_Ntk_t *)Vec_PtrEntry(pLib->vNtks, i); }
static inline Rtl_Ntk_t * Rtl_LibTop( Rtl_Lib_t * pLib )                   { return Rtl_LibNtk( pLib, Rtl_LibNtkNum(pLib)-1 ); }

static inline Rtl_Ntk_t * Rtl_NtkModule( Rtl_Ntk_t * p, int i )            { return Rtl_LibNtk( p->pLib, i );                  }

static inline int         Rtl_NtkStrId( Rtl_Ntk_t * p, char * s )          { return Abc_NamStrFind(p->pLib->pManName, s);      }
static inline char *      Rtl_NtkStr( Rtl_Ntk_t * p, int h )               { return Abc_NamStr(p->pLib->pManName, h);          }
static inline char *      Rtl_NtkName( Rtl_Ntk_t * p )                     { return Rtl_NtkStr(p, p->NameId);                  }

static inline FILE *      Rtl_NtkFile( Rtl_Ntk_t * p )                     { return p->pLib->pFile;                            }
static inline int         Rtl_NtkTokId( Rtl_Ntk_t * p, int i )             { return i < Vec_IntSize(p->pLib->vTokens) ? Vec_IntEntry(p->pLib->vTokens, i) : -1;                  }
static inline char *      Rtl_NtkTokStr( Rtl_Ntk_t * p, int i )            { return i < Vec_IntSize(p->pLib->vTokens) ? Rtl_NtkStr(p, Vec_IntEntry(p->pLib->vTokens, i)) : NULL; }
static inline int         Rtl_NtkTokCheck( Rtl_Ntk_t * p, int i, int Tok ) { return i == p->pLib->pMap[Tok];                                    }
static inline int         Rtl_NtkPosCheck( Rtl_Ntk_t * p, int i, int Tok ) { return Vec_IntEntry(p->pLib->vTokens, i) == p->pLib->pMap[Tok];    }

static inline int         Rtl_NtkInputNum( Rtl_Ntk_t * p )                 { return p->nInputs;                                }
static inline int         Rtl_NtkOutputNum( Rtl_Ntk_t * p )                { return p->nOutputs;                               }
static inline int         Rtl_NtkAttrNum( Rtl_Ntk_t * p )                  { return Vec_IntSize(&p->vAttrs)/2;                 }
static inline int         Rtl_NtkWireNum( Rtl_Ntk_t * p )                  { return Vec_IntSize(&p->vWires)/WIRE_NUM;          }
static inline int         Rtl_NtkCellNum( Rtl_Ntk_t * p )                  { return Vec_IntSize(&p->vCells);                   }
static inline int         Rtl_NtkConNum( Rtl_Ntk_t * p )                   { return Vec_IntSize(&p->vConns)/2;                 }
static inline int         Rtl_NtkObjNum( Rtl_Ntk_t * p )                   { return p->nInputs + p->nOutputs + Rtl_NtkCellNum(p) + Rtl_NtkConNum(p); }

static inline int *       Rtl_NtkWire( Rtl_Ntk_t * p, int i )              { return Vec_IntEntryP(&p->vWires, WIRE_NUM*i);                  }
static inline int *       Rtl_NtkCell( Rtl_Ntk_t * p, int i )              { return Vec_IntEntryP(&p->vStore, Vec_IntEntry(&p->vCells, i)); }
static inline int *       Rtl_NtkCon( Rtl_Ntk_t * p, int i )               { return Vec_IntEntryP(&p->vConns, 2*i);                         }

static inline int         Rtl_WireName( Rtl_Ntk_t * p, int i )             { return Vec_IntEntry(&p->vWires, WIRE_NUM*i) >> 4; }
static inline char *      Rtl_WireNameStr( Rtl_Ntk_t * p, int i )          { return Rtl_NtkStr(p, Rtl_WireName(p, i));         }
static inline int         Rtl_WireFirst( Rtl_Ntk_t * p, int i )            { return Vec_IntEntry(&p->vWires, WIRE_NUM*i);      }
static inline int         Rtl_WireWidth( Rtl_Ntk_t * p, int i )            { return Vec_IntEntry(&p->vWires, WIRE_NUM*i+1);    }
static inline int         Rtl_WireOffset( Rtl_Ntk_t * p, int i )           { return Vec_IntEntry(&p->vWires, WIRE_NUM*i+2);    }
static inline int         Rtl_WireNumber( Rtl_Ntk_t * p, int i )           { return Vec_IntEntry(&p->vWires, WIRE_NUM*i+3);    }
static inline int         Rtl_WireBitStart( Rtl_Ntk_t * p, int i )         { return Vec_IntEntry(&p->vWires, WIRE_NUM*i+4);    }
static inline int         Rtl_WireMapNameToId( Rtl_Ntk_t * p, int i )      { return Vec_IntEntry(p->pLib->vMap, i);            }

static inline int         Rtl_CellType( int * pCell )                      { return pCell[0];                                  }
static inline int         Rtl_CellName( int * pCell )                      { return pCell[1];                                  }
static inline int         Rtl_CellModule( int * pCell )                    { return pCell[2];                                  }
static inline int         Rtl_CellInputNum( int * pCell )                  { return pCell[3];                                  }
static inline int         Rtl_CellOutputNum( int * pCell )                 { return pCell[6]-pCell[3];                         }
static inline int         Rtl_CellAttrNum( int * pCell )                   { return pCell[4];                                  }
static inline int         Rtl_CellParamNum( int * pCell )                  { return pCell[5];                                  }
static inline int         Rtl_CellConNum( int * pCell )                    { return pCell[6];                                  }
static inline int         Rtl_CellMark( int * pCell )                      { return pCell[7];                                  }
static inline Rtl_Ntk_t * Rtl_CellNtk( Rtl_Ntk_t * p, int * pCell )        { return Rtl_CellModule(pCell) >= ABC_INFINITY ? Rtl_NtkModule(p, Rtl_CellModule(pCell)-ABC_INFINITY) : NULL; }

static inline char *      Rtl_CellTypeStr( Rtl_Ntk_t * p, int * pCell )    { return Rtl_NtkStr(p, Rtl_CellType(pCell));        }
static inline char *      Rtl_CellNameStr( Rtl_Ntk_t * p, int * pCell )    { return Rtl_NtkStr(p, Rtl_CellName(pCell));        }

static inline int         Rtl_SigIsNone( int s )                           { return (s & 0x3) == 0;                            }
static inline int         Rtl_SigIsConst( int s )                          { return (s & 0x3) == 1;                            }
static inline int         Rtl_SigIsSlice( int s )                          { return (s & 0x3) == 2;                            }
static inline int         Rtl_SigIsConcat( int s )                         { return (s & 0x3) == 3;                            }

#define Rtl_NtkForEachAttr( p, Par, Val, i ) \
    for ( i = 0; i < Rtl_NtkAttrNum(p) && (Par = Vec_IntEntry(&p->vAttrs, 2*i)) && (Val = Vec_IntEntry(&p->vAttrs, 2*i+1)); i++ )
#define Rtl_NtkForEachWire( p, pWire, i ) \
    for ( i = 0; i < Rtl_NtkWireNum(p) && (pWire = Vec_IntEntryP(&p->vWires, WIRE_NUM*i)); i++ )
#define Rtl_NtkForEachCell( p, pCell, i ) \
    for ( i = 0; i < Rtl_NtkCellNum(p) && (pCell = Rtl_NtkCell(p, i)); i++ )
#define Rtl_NtkForEachCon( p, pCon, i ) \
    for ( i = 0; i < Rtl_NtkConNum(p) && (pCon = Vec_IntEntryP(&p->vConns, 2*i)); i++ )

#define Rtl_CellForEachAttr( p, pCell, Par, Val, i ) \
    for ( i = 0; i < pCell[4] && (Par = pCell[CELL_NUM+2*i]) && (Val = pCell[CELL_NUM+2*i+1]); i++ )
#define Rtl_CellForEachParam( p, pCell, Par, Val, i ) \
    for ( i = 0; i < pCell[5] && (Par = pCell[CELL_NUM+2*(pCell[4]+i)]) && (Val = pCell[CELL_NUM+2*(pCell[4]+i)+1]); i++ )
#define Rtl_CellForEachConnect( p, pCell, Par, Val, i ) \
    for ( i = 0; i < pCell[6] && (Par = pCell[CELL_NUM+2*(pCell[4]+pCell[5]+i)]) && (Val = pCell[CELL_NUM+2*(pCell[4]+pCell[5]+i)+1]); i++ )

#define Rtl_CellForEachInput( p, pCell, Par, Val, i ) \
    Rtl_CellForEachConnect( p, pCell, Par, Val, i ) if ( i >= Rtl_CellInputNum(pCell) ) continue; else
#define Rtl_CellForEachOutput( p, pCell, Par, Val, i ) \
    Rtl_CellForEachConnect( p, pCell, Par, Val, i ) if ( i <  Rtl_CellInputNum(pCell) ) continue; else

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Rtl_Ntk_t * Rtl_NtkAlloc( Rtl_Lib_t * pLib )
{
    Rtl_Ntk_t * p = ABC_CALLOC( Rtl_Ntk_t, 1 );
    Vec_IntGrow( &p->vWires, 4 );
    Vec_IntGrow( &p->vCells, 4 );
    Vec_IntGrow( &p->vConns, 4 );
    Vec_IntGrow( &p->vStore, 8 );
    Vec_IntGrow( &p->vAttrs, 8 );
    Vec_PtrPush( pLib->vNtks, (void *)p );
    p->pLib = pLib;
    return p;
}
void Rtl_NtkFree( Rtl_Ntk_t * p )
{
    Gia_ManStopP( &p->pGia );
    ABC_FREE( p->vWires.pArray );
    ABC_FREE( p->vCells.pArray );
    ABC_FREE( p->vConns.pArray );
    ABC_FREE( p->vStore.pArray );
    ABC_FREE( p->vAttrs.pArray );
    ABC_FREE( p->vOrder.pArray );
    ABC_FREE( p->vLits.pArray );
    ABC_FREE( p->vBitTemp.pArray );
    ABC_FREE( p );
}
void Rtl_NtkCountPio( Rtl_Ntk_t * p, int Counts[4] )
{
    int i, * pWire;
    Rtl_NtkForEachWire( p, pWire, i )
    {
        if ( pWire[0] & 1 ) // PI
            Counts[0]++, Counts[1] += pWire[1];
        if ( pWire[0] & 2 ) // PO
            Counts[2]++, Counts[3] += pWire[1];
    }
    assert( p->nInputs  == Counts[0] );
    assert( p->nOutputs == Counts[2] );
}
void Rtl_NtkPrintOpers( Rtl_Ntk_t * p )
{
    int i, * pCell, nBlack = 0, nUser = 0, Counts[ABC_OPER_LAST] = {0};
    if ( Rtl_NtkCellNum(p) == 0 )
        return;
    Rtl_NtkForEachCell( p, pCell, i )
        if ( Rtl_CellModule(pCell) < ABC_OPER_LAST )
            Counts[Rtl_CellModule(pCell)]++;
        else if ( Rtl_CellModule(pCell) == ABC_OPER_LAST-1 )
            nBlack++;
        else
            nUser++;
    printf( "There are %d instances in this network:\n", Rtl_NtkCellNum(p) );
    if ( nBlack )
        printf( "  %s (%d)", "blackbox", nBlack );
    if ( nUser )
        printf( "  %s (%d)", "user", nUser );
    for ( i = 0; i < ABC_OPER_LAST; i++ )
        if ( Counts[i] )
            printf( "  %s (%d)", Abc_OperName(i), Counts[i] );
    printf( "\n" );
}
void Rtl_NtkPrintStats( Rtl_Ntk_t * p, int nNameSymbs )
{
    int Counts[4] = {0};     Rtl_NtkCountPio( p, Counts );
    printf( "%*s : ",        nNameSymbs, Rtl_NtkName(p) );
    printf( "PI = %3d (%3d)  ", Counts[0], Counts[1] );
    printf( "PO = %3d (%3d)  ", Counts[2], Counts[3] );
    printf( "Wire = %6d   ", Rtl_NtkWireNum(p) );
    printf( "Cell = %6d   ", Rtl_NtkCellNum(p) );
    printf( "Con = %6d",     Rtl_NtkConNum(p) );
    printf( "\n" );
    //Rtl_NtkPrintOpers( p );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Rtl_Lib_t * Rtl_LibAlloc()
{
    Rtl_Lib_t * p = ABC_CALLOC( Rtl_Lib_t, 1 );
    p->vNtks = Vec_PtrAlloc( 100 );
    Vec_IntGrow( &p->vConsts,  1000 );
    Vec_IntGrow( &p->vSlices,  1000 );
    Vec_IntGrow( &p->vConcats, 1000 );
    return p;
}
void Rtl_LibFree( Rtl_Lib_t * p )
{
    Rtl_Ntk_t * pNtk; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        Rtl_NtkFree( pNtk );    
    ABC_FREE( p->vConsts.pArray );
    ABC_FREE( p->vSlices.pArray );
    ABC_FREE( p->vConcats.pArray );
    ABC_FREE( p->vAttrTemp.pArray );
    for ( i = 0; i < TEMP_NUM; i++ )
        ABC_FREE( p->vTemp[i].pArray );
    Vec_IntFreeP( &p->vMap );
    Vec_IntFreeP( &p->vTokens );
    Abc_NamStop( p->pManName );
    Vec_PtrFree( p->vNtks );
    ABC_FREE( p->pSpec );
    ABC_FREE( p );
}
int Rtl_LibFindModule( Rtl_Lib_t * p, int NameId )
{
    Rtl_Ntk_t * pNtk; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        if ( pNtk->NameId == NameId )
            return i;
    return -1;
}
void Rtl_LibPrintStats( Rtl_Lib_t * p )
{
    Rtl_Ntk_t * pNtk; int i, nSymbs = 0;
    printf( "Modules found in \"%s\":\n", p->pSpec );
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        nSymbs = Abc_MaxInt( nSymbs, strlen(Rtl_NtkName(pNtk)) );
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        Rtl_NtkPrintStats( pNtk, nSymbs + 2 );  
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
typedef enum {
    RTL_NONE = 0,  // 0:  unused
    RTL_MODULE,    // 1:  "module"
    RTL_END,       // 2:  "end"
    RTL_INPUT,     // 3:  "input"
    RTL_OUTPUT,    // 4:  "output"
    RTL_INOUT,     // 5:  "inout"
    RTL_UPTO,      // 6:  "upto"
    RTL_SIGNED,    // 7:  "signed"
    RTL_OFFSET,    // 8:  "offset"
    RTL_PARAMETER, // 9:  "parameter"
    RTL_WIRE,      // 10: "wire"
    RTL_CONNECT,   // 11: "connect"
    RTL_CELL,      // 12: "cell"
    RTL_WIDTH,     // 13: "width"
    RTL_ATTRIBUTE, // 14: "attribute"
    RTL_UNUSED     // 15: unused
} Rtl_Type_t; 

static inline char * Rtl_Num2Name( int i )
{
    if ( i == 1  )  return "module";
    if ( i == 2  )  return "end";
    if ( i == 3  )  return "input";
    if ( i == 4  )  return "output";
    if ( i == 5  )  return "inout";
    if ( i == 6  )  return "upto";
    if ( i == 7  )  return "signed";
    if ( i == 8  )  return "offset";
    if ( i == 9  )  return "parameter";
    if ( i == 10 )  return "wire";
    if ( i == 11 )  return "connect";
    if ( i == 12 )  return "cell";
    if ( i == 13 )  return "width";
    if ( i == 14 )  return "attribute";
    return NULL;
}

static inline void Rtl_LibDeriveMap( Rtl_Lib_t * p )
{
    int i;
    p->pMap[0] = -1;
    for ( i = 1; i < RTL_UNUSED; i++ )
        p->pMap[i] = Abc_NamStrFind( p->pManName, Rtl_Num2Name(i) );
}



/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rtl_LibReadType( char * pType )
{
    if ( !strcmp(pType, "$not") )         return ABC_OPER_BIT_INV;       // Y = ~A       $not            
    if ( !strcmp(pType, "$pos") )         return ABC_OPER_BIT_BUF;       // Y = +A       $pos            
    if ( !strcmp(pType, "$neg") )         return ABC_OPER_ARI_MIN;       // Y = -A       $neg            
    if ( !strcmp(pType, "$reduce_and") )  return ABC_OPER_RED_AND;       // Y = &A       $reduce_and     
    if ( !strcmp(pType, "$reduce_or") )   return ABC_OPER_RED_OR;        // Y = |A       $reduce_or      
    if ( !strcmp(pType, "$reduce_xor") )  return ABC_OPER_RED_XOR;       // Y = ^A       $reduce_xor     
    if ( !strcmp(pType, "$reduce_xnor") ) return ABC_OPER_RED_NXOR;      // Y = ~^A      $reduce_xnor   
    if ( !strcmp(pType, "$reduce_bool") ) return ABC_OPER_RED_OR;        // Y = |A       $reduce_bool    
    if ( !strcmp(pType, "$logic_not") )   return ABC_OPER_LOGIC_NOT;     // Y = !A       $logic_not      

    if ( !strcmp(pType, "$and") )         return ABC_OPER_BIT_AND;       // Y = A & B    $and         
    if ( !strcmp(pType, "$or") )          return ABC_OPER_BIT_OR;        // Y = A | B    $or          
    if ( !strcmp(pType, "$xor") )         return ABC_OPER_BIT_XOR;       // Y = A ^ B    $xor         
    if ( !strcmp(pType, "$xnor") )        return ABC_OPER_BIT_NXOR;      // Y = A ~^ B   $xnor    
    
    if ( !strcmp(pType, "$shl") )         return ABC_OPER_SHIFT_L;       // Y = A << B   $shl        
    if ( !strcmp(pType, "$shr") )         return ABC_OPER_SHIFT_R;       // Y = A >> B   $shr        
    if ( !strcmp(pType, "$sshl") )        return ABC_OPER_SHIFT_LA;      // Y = A <<< B  $sshl      
    if ( !strcmp(pType, "$sshr") )        return ABC_OPER_SHIFT_RA;      // Y = A >>> B  $sshr      

    if ( !strcmp(pType, "$shiftx") )      return ABC_OPER_SHIFT_R;       // Y = A << B   $shl     <== temporary   

    if ( !strcmp(pType, "$logic_and") )   return ABC_OPER_LOGIC_AND;     // Y = A && B   $logic_and  
    if ( !strcmp(pType, "$logic_or") )    return ABC_OPER_LOGIC_OR;      // Y = A || B   $logic_or  
    
    if ( !strcmp(pType, "$lt") )          return ABC_OPER_COMP_LESS;     // Y = A < B    $lt          
    if ( !strcmp(pType, "$le") )          return ABC_OPER_COMP_LESSEQU;  // Y = A <= B   $le         
    if ( !strcmp(pType, "$ge") )          return ABC_OPER_COMP_MOREEQU;  // Y = A >= B   $ge           
    if ( !strcmp(pType, "$gt") )          return ABC_OPER_COMP_MORE;     // Y = A > B    $gt        
    if ( !strcmp(pType, "$eq") )          return ABC_OPER_COMP_EQU;      // Y = A == B   $eq         
    if ( !strcmp(pType, "$ne") )          return ABC_OPER_COMP_NOTEQU;   // Y = A != B   $ne         
    if ( !strcmp(pType, "$eqx") )         return ABC_OPER_COMP_EQU;      // Y = A === B  $eqx       
    if ( !strcmp(pType, "$nex") )         return ABC_OPER_COMP_NOTEQU;   // Y = A !== B  $nex       
    
    if ( !strcmp(pType, "$add") )         return ABC_OPER_ARI_ADD;       // Y = A + B    $add         
    if ( !strcmp(pType, "$sub") )         return ABC_OPER_ARI_SUB;       // Y = A - B    $sub         
    if ( !strcmp(pType, "$mul") )         return ABC_OPER_ARI_MUL;       // Y = A * B    $mul         
    if ( !strcmp(pType, "$div") )         return ABC_OPER_ARI_DIV;       // Y = A / B    $div         
    if ( !strcmp(pType, "$mod") )         return ABC_OPER_ARI_MOD;       // Y = A % B    $mod         
    if ( !strcmp(pType, "$pow") )         return ABC_OPER_ARI_POW;       // Y = A ** B   $pow        

    if ( !strcmp(pType, "$modfoor") )     return ABC_OPER_NONE;          // [N/A] $modfoor         
    if ( !strcmp(pType, "$divfloor") )    return ABC_OPER_NONE;          // [N/A] $divfloor        

    if ( !strcmp(pType, "$mux") )         return ABC_OPER_SEL_NMUX;      // $mux                   
    if ( !strcmp(pType, "$pmux") )        return ABC_OPER_SEL_SEL;       // $pmux                  
                                                               
    if ( !strcmp(pType, "$dff") )         return ABC_OPER_DFF;
    if ( !strcmp(pType, "$adff") )        return ABC_OPER_DFF;
    if ( !strcmp(pType, "$sdff") )        return ABC_OPER_DFF;
    assert( 0 );                                               
    return -1;                                                 
}                                                                         
int Rtl_NtkReadType( Rtl_Ntk_t * p, int Type )                                                                         
{                                                                         
    extern int Rtl_LibFindModule( Rtl_Lib_t * p, int NameId );
    char * pType = Rtl_NtkStr( p, Type );                   
    if ( pType[0] == '$' && strncmp(pType,"$paramod",strlen("$paramod")) )
        return Rtl_LibReadType( pType );                    
    return ABC_INFINITY + Rtl_LibFindModule( p->pLib, Type );
}                                                           
                                                            
/**Function*************************************************************
                                                            
  Synopsis    [There is no need to normalize ranges in Yosys.]
                                                            
  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rtl_NtkRangeWires( Rtl_Ntk_t * p )
{
    int i, * pWire, nBits = 0;
    Rtl_NtkForEachWire( p, pWire, i )
    {
        //printf( "%s -> %d\n", Rtl_WireNameStr(p, i), nBits );
        pWire[4] = nBits, nBits += Rtl_WireWidth(p, i);
    }
    return nBits;
}
void Rtl_NtkMapWires( Rtl_Ntk_t * p, int fUnmap )
{
    int i, Value;
    assert( Vec_IntSize(p->pLib->vMap) == Abc_NamObjNumMax(p->pLib->pManName) );
    for ( i = 0; i < Rtl_NtkWireNum(p); i++ )
    {
        int NameId = Rtl_WireName( p, i );
        assert( Vec_IntEntry(p->pLib->vMap, NameId) == (fUnmap ? i : -1) );
        Vec_IntWriteEntry( p->pLib->vMap, NameId, fUnmap ? -1 : i );
    }
    if ( fUnmap )
        Vec_IntForEachEntry( p->pLib->vMap, Value, i )
            assert( Value == -1 );
}
void Rtl_NtkNormRanges( Rtl_Ntk_t * p )
{
    int i, * pWire;
    Rtl_NtkMapWires( p, 0 );
    for ( i = p->Slice0; i < p->Slice1; i += 3 )
    {
        int NameId = Vec_IntEntry( &p->pLib->vSlices, i );
        int Left   = Vec_IntEntry( &p->pLib->vSlices, i+1 );
        int Right  = Vec_IntEntry( &p->pLib->vSlices, i+2 );
        int Wire   = Rtl_WireMapNameToId( p, NameId );
        int Offset = Rtl_WireOffset( p, Wire );
        int First  = Rtl_WireFirst( p, Wire );
        assert( First >> 4 == NameId );
        if ( Offset );
        {
            Left  -= Offset;
            Right -= Offset;
        }
        if ( First & 8 ) // upto
        {
            Vec_IntWriteEntry( &p->pLib->vSlices, i+1, Right );
            Vec_IntWriteEntry( &p->pLib->vSlices, i+2, Left );
        }
    }
    Rtl_NtkForEachWire( p, pWire, i )
    {
        Vec_IntWriteEntry( &p->vWires, WIRE_NUM*i+0, Rtl_WireFirst(p, i) & ~0x8 ); // upto
        Vec_IntWriteEntry( &p->vWires, WIRE_NUM*i+2, 0 ); // offset
    }
    Rtl_NtkMapWires( p, 1 );
}
void Rtl_LibNormRanges( Rtl_Lib_t * pLib )
{
    Rtl_Ntk_t * p; int i;
    if ( pLib->vMap == NULL )
        pLib->vMap = Vec_IntStartFull( Abc_NamObjNumMax(pLib->pManName) );
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        Rtl_NtkNormRanges( p );    
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int * Rlt_NtkFindIOPerm( Rtl_Ntk_t * p )
{
    Vec_Int_t * vCost = Vec_IntAlloc( 100 );
    int i, * pWire, * pPerm = NULL, Count = 0;
    Rtl_NtkForEachWire( p, pWire, i )
    {
        int First  = Rtl_WireFirst( p, i );
        int Number = Rtl_WireNumber( p, i );
        int fIsPi  = (int)((First & 1) > 0);
        int fIsPo  = (int)((First & 2) > 0);
        assert( (fIsPi || fIsPo) == (Number > 0) );
        if ( fIsPi || fIsPo )
            Vec_IntPush( vCost, fIsPo*ABC_INFINITY + Number );
        else
            Vec_IntPush( vCost, 2*ABC_INFINITY + Count++ );
    }
    pPerm = Abc_MergeSortCost( Vec_IntArray(vCost), Vec_IntSize(vCost) );
    Vec_IntFree( vCost );
    return pPerm;
}
void Rtl_NtkOrderWires( Rtl_Ntk_t * p )
{
    Vec_Int_t * vTemp = Vec_IntAlloc( Vec_IntSize(&p->vWires) );
    int i, k, * pWire, * pPerm = Rlt_NtkFindIOPerm( p );
    Rtl_NtkForEachWire( p, pWire, i )
    {
        pWire = Vec_IntEntryP( &p->vWires, WIRE_NUM*pPerm[i] );
        for ( k = 0; k < WIRE_NUM; k++ )
            Vec_IntPush( vTemp, pWire[k] );
    }
    ABC_FREE( pPerm );
    assert( Vec_IntSize(&p->vWires) == Vec_IntSize(vTemp) );
    ABC_SWAP( Vec_Int_t, p->vWires, *vTemp ); 
    Vec_IntFree( vTemp );
}
void Rtl_LibUpdateInstances( Rtl_Ntk_t * p )
{
    Vec_Int_t * vMap  = p->pLib->vMap;
    Vec_Int_t * vTemp = &p->pLib->vTemp[2];
    int i, k, Par, Val, * pCell, Value;
    Rtl_NtkForEachCell( p, pCell, i )
        if ( Rtl_CellModule(pCell) >= ABC_INFINITY )
        {
            Rtl_Ntk_t * pModel = Rtl_NtkModule( p, Rtl_CellModule(pCell)-ABC_INFINITY );
            assert( pCell[6] == pModel->nInputs+pModel->nOutputs );
            Rtl_CellForEachConnect( p, pCell, Par, Val, k )
                Vec_IntWriteEntry( vMap, Par >> 2, k );
            Vec_IntClear( vTemp );
            for ( k = 0; k < pCell[6]; k++ )
            {
                int Perm = Vec_IntEntry( vMap, Rtl_WireName(pModel, k) );
                int Par = pCell[CELL_NUM+2*(pCell[4]+pCell[5]+Perm)];
                int Val = pCell[CELL_NUM+2*(pCell[4]+pCell[5]+Perm)+1];
                assert( (Par >> 2) == Rtl_WireName(pModel, k) );
                Vec_IntWriteEntry( vMap, Par >> 2, -1 );
                Vec_IntPushTwo( vTemp, Par, Val );
                assert( Perm >= 0 );
            }
            memcpy( pCell+CELL_NUM+2*(pCell[4]+pCell[5]), Vec_IntArray(vTemp), sizeof(int)*Vec_IntSize(vTemp) );
        }
    Vec_IntForEachEntry( p->pLib->vMap, Value, i )
        assert( Value == -1 );
}
void Rtl_LibOrderWires( Rtl_Lib_t * pLib )
{
    Rtl_Ntk_t * p; int i;
    if ( pLib->vMap == NULL )
        pLib->vMap = Vec_IntStartFull( Abc_NamObjNumMax(pLib->pManName) );
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        Rtl_NtkOrderWires( p );    
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        Rtl_LibUpdateInstances( p );    
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
extern int Rtl_NtkCountSignalRange( Rtl_Ntk_t * p, int Sig );

int Rtl_NtkCountWireRange( Rtl_Ntk_t * p, int NameId )
{
    int Wire  = Rtl_WireMapNameToId( p, NameId );
    int Width = Rtl_WireWidth( p, Wire );
    return Width;
}
int Rtl_NtkCountSliceRange( Rtl_Ntk_t * p, int * pSlice )
{
    return pSlice[1] - pSlice[2] + 1;
}
int Rtl_NtkCountConcatRange( Rtl_Ntk_t * p, int * pConcat )
{
    int i, nBits = 0;
    for ( i = 1; i <= pConcat[0]; i++ )
        nBits += Rtl_NtkCountSignalRange( p, pConcat[i] );
    return nBits;
}
int Rtl_NtkCountSignalRange( Rtl_Ntk_t * p, int Sig )
{
    if ( Rtl_SigIsNone(Sig) )
        return Rtl_NtkCountWireRange( p, Sig >> 2 );
    if ( Rtl_SigIsSlice(Sig) )
        return Rtl_NtkCountSliceRange( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2) );
    if ( Rtl_SigIsConcat(Sig) )
        return Rtl_NtkCountConcatRange( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2) );
    if ( Rtl_SigIsConst(Sig) )
        assert( 0 );
    return ABC_INFINITY;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
extern int Rtl_NtkCheckSignalRange( Rtl_Ntk_t * p, int Sig );

int Rtl_NtkCheckWireRange( Rtl_Ntk_t * p, int NameId, int Left, int Right )
{
    int Wire  = Rtl_WireMapNameToId( p, NameId );
    int First = Rtl_WireBitStart( p, Wire );
    int Width = Rtl_WireWidth( p, Wire ), i;
    Left  = Left  == -1 ? Width-1 :  Left;
    Right = Right == -1 ? 0       : Right;
    assert ( Right <= Left && Right >= 0 );
    for ( i = Right; i <= Left; i++ )
        if ( Vec_IntEntry(&p->vLits, First+i) == -1 )
            return 0;
    return 1;
}
int Rtl_NtkCheckSliceRange( Rtl_Ntk_t * p, int * pSlice )
{
    return Rtl_NtkCheckWireRange( p, pSlice[0], pSlice[1], pSlice[2] );
}
int Rtl_NtkCheckConcatRange( Rtl_Ntk_t * p, int * pConcat )
{
    int i;
    for ( i = 1; i <= pConcat[0]; i++ )
        if ( !Rtl_NtkCheckSignalRange( p, pConcat[i] ) )
            return 0;
    return 1;
}
int Rtl_NtkCheckSignalRange( Rtl_Ntk_t * p, int Sig )
{
    if ( Rtl_SigIsNone(Sig) )
        return Rtl_NtkCheckWireRange( p, Sig >> 2, -1, -1 );
    else if ( Rtl_SigIsConst(Sig) )
        return 1;
    else if ( Rtl_SigIsSlice(Sig) )
        return Rtl_NtkCheckSliceRange( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2) );
    else if ( Rtl_SigIsConcat(Sig) )
        return Rtl_NtkCheckConcatRange( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2) );
    else assert( 0 );
    return -1;
}


extern void Rtl_NtkSetSignalRange( Rtl_Ntk_t * p, int Sig, int Value );

void Rtl_NtkSetWireRange( Rtl_Ntk_t * p, int NameId, int Left, int Right, int Value )
{
    //char * pName = Rtl_NtkStr( p, NameId );
    int Wire  = Rtl_WireMapNameToId( p, NameId );
    int First = Rtl_WireBitStart( p, Wire );
    int Width = Rtl_WireWidth( p, Wire ), i;
    Left  = Left  == -1 ? Width-1 :  Left;
    Right = Right == -1 ? 0       : Right;
    assert ( Right <= Left && Right >= 0 );
    for ( i = Right; i <= Left; i++ )
    {
        assert( Vec_IntEntry(&p->vLits, First+i) == -1 );
        Vec_IntWriteEntry(&p->vLits, First+i, Value );
    }
    //printf( "Finished setting wire %s\n", Rtl_NtkStr(p, NameId) );
}
void Rtl_NtkSetSliceRange( Rtl_Ntk_t * p, int * pSlice, int Value )
{
    Rtl_NtkSetWireRange( p, pSlice[0], pSlice[1], pSlice[2], Value );
}
void Rtl_NtkSetConcatRange( Rtl_Ntk_t * p, int * pConcat, int Value )
{
    int i;
    for ( i = 1; i <= pConcat[0]; i++ )
        Rtl_NtkSetSignalRange( p, pConcat[i], Value );
}
void Rtl_NtkSetSignalRange( Rtl_Ntk_t * p, int Sig, int Value )
{
    if ( Rtl_SigIsNone(Sig) )
        Rtl_NtkSetWireRange( p, Sig >> 2, -1, -1, Value );
    else if ( Rtl_SigIsSlice(Sig) )
        Rtl_NtkSetSliceRange( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2), Value );
    else if ( Rtl_SigIsConcat(Sig) )
        Rtl_NtkSetConcatRange( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2), Value );
    else if ( Rtl_SigIsConst(Sig) )
        assert( 0 );
}


void Rtl_NtkInitInputs( Rtl_Ntk_t * p )
{
    int b, i;
    for ( i = 0; i < p->nInputs; i++ )
    {
        int First = Rtl_WireBitStart( p, i );
        int Width = Rtl_WireWidth( p, i );
        for ( b = 0; b < Width; b++ )
        {
            assert( Vec_IntEntry(&p->vLits, First+b) == -1 );
            Vec_IntWriteEntry( &p->vLits, First+b, Vec_IntSize(&p->vOrder) );
        }
        Vec_IntPush( &p->vOrder, i );
        //printf( "Finished setting input %s\n", Rtl_WireNameStr(p, i) );
    }
}
Vec_Int_t * Rtl_NtkCollectOutputs( Rtl_Ntk_t * p )
{
    //char * pNtkName = Rtl_NtkName(p);
    int b, i;
    Vec_Int_t * vRes = Vec_IntAlloc( 100 );
    for ( i = 0; i < p->nOutputs; i++ )
    {
        //char * pName = Rtl_WireNameStr(p, p->nInputs + i);
        int First = Rtl_WireBitStart( p, p->nInputs + i );
        int Width = Rtl_WireWidth( p, p->nInputs + i );
        for ( b = 0; b < Width; b++ )
        {
            assert( Vec_IntEntry(&p->vLits, First+b) != -1 );
            Vec_IntPush( vRes, Vec_IntEntry(&p->vLits, First+b) );
        }
    }
    return vRes;
}
int Rtl_NtkReviewCells( Rtl_Ntk_t * p )
{
    int i, k, Par, Val, * pCell, RetValue = 0;
    Rtl_NtkForEachCell( p, pCell, i )
    {
        if ( pCell[7] )
            continue;
        Rtl_CellForEachInput( p, pCell, Par, Val, k )
            if ( !Rtl_NtkCheckSignalRange( p, Val ) )
                break;
        if ( k < Rtl_CellInputNum(pCell) )
            continue;
        Rtl_CellForEachOutput( p, pCell, Par, Val, k )
            Rtl_NtkSetSignalRange( p, Val, Vec_IntSize(&p->vOrder) );
        Vec_IntPush( &p->vOrder, p->nInputs + i );
        pCell[7] = 1;
        RetValue = 1;
        //printf( "Setting cell %s as propagated.\n", Rtl_CellNameStr(p, pCell) );
    }
    return RetValue;
}
int Rtl_NtkReviewConnections( Rtl_Ntk_t * p )
{
    int i, * pCon, RetValue = 0;
    Rtl_NtkForEachCon( p, pCon, i )
    {
        int Status0 = Rtl_NtkCheckSignalRange( p, pCon[0] );
        int Status1 = Rtl_NtkCheckSignalRange( p, pCon[1] );
        if ( Status0 == Status1 )
            continue;
        if ( !Status0 && Status1 )
            ABC_SWAP( int, pCon[0], pCon[1] )
        Rtl_NtkSetSignalRange( p, pCon[1], Vec_IntSize(&p->vOrder) );
        Vec_IntPush( &p->vOrder, p->nInputs + Rtl_NtkCellNum(p) + i );
        RetValue = 1;
    }
    return RetValue;
}
void Rtl_NtkPrintCellOrder( Rtl_Ntk_t * p )
{
    int i, iCell;
    Vec_IntForEachEntry( &p->vOrder, iCell, i )
    {
        printf( "%4d :  ", i );
        printf( "Cell %4d  ", iCell );
        if ( iCell < p->nInputs )
            printf( "Type  Input " );
        else if ( iCell < p->nInputs + Rtl_NtkCellNum(p) )
        {
            int * pCell = Rtl_NtkCell( p, iCell - p->nInputs );
            printf( "Type  %4d  ", Rtl_CellType(pCell) );
            printf( "%16s ",       Rtl_CellTypeStr(p, pCell) );
            printf( "%16s ",       Rtl_CellNameStr(p, pCell) );
        }
        else
            printf( "Type  Connection " );
        printf( "\n" );
    }
}
void Rtl_NtkPrintUnusedCells( Rtl_Ntk_t * p )
{
    int i, * pCell;
    printf( "\n*** Printing unused cells:\n" );
    Rtl_NtkForEachCell( p, pCell, i )
    {
        if ( pCell[7] )
            continue;
        printf( "Unused cell %s           %s\n", Rtl_CellTypeStr(p, pCell), Rtl_CellNameStr(p, pCell) );
    }
    printf( "\n" );
}
void Rtl_NtkOrderCells( Rtl_Ntk_t * p )
{
    Vec_Int_t * vRes;
    int nBits = Rtl_NtkRangeWires( p );
    Vec_IntFill( &p->vLits, nBits, -1 );

    Vec_IntClear( &p->vOrder );
    Vec_IntGrow( &p->vOrder, Rtl_NtkObjNum(p) );
    Rtl_NtkInitInputs( p );

    Rtl_NtkMapWires( p, 0 );
//Vec_IntPrint(&p->vLits);

    Rtl_NtkReviewConnections( p );
    while ( Rtl_NtkReviewCells(p) | Rtl_NtkReviewConnections(p) );
    Rtl_NtkMapWires( p, 1 );

    vRes = Rtl_NtkCollectOutputs( p );
    Vec_IntFree( vRes );

    //Rtl_NtkPrintCellOrder( p );
}
void Rtl_LibOrderCells( Rtl_Lib_t * pLib )
{
    Rtl_Ntk_t * p; int i;
    if ( pLib->vMap == NULL )
        pLib->vMap = Vec_IntStartFull( Abc_NamObjNumMax(pLib->pManName) );
    assert( Vec_IntSize(pLib->vMap) == Abc_NamObjNumMax(pLib->pManName) );
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        Rtl_NtkOrderCells( p );    
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rtl_TokenUnspace( char * p )
{
    int i, Length = strlen(p), Quote = 0;
    for ( i = 0; i < Length; i++ )
        if ( p[i] == '\"' )
            Quote ^= 1;
        else if ( Quote && p[i] == ' ' )
            p[i] = '\"';
}
void Rtl_TokenRespace( char * p )
{
    int i, Length = strlen(p);
    assert( p[0] == '\"' && p[Length-1] == '\"' );
    for ( i = 1; i < Length-1; i++ )
        if ( p[i] == '\"' )
            p[i] = ' ';
}
Vec_Int_t * Rtl_NtkReadFile( char * pFileName, Abc_Nam_t * p )
{
    Vec_Int_t * vTokens;
    char * pTemp, Buffer[MAX_LINE]; 
    FILE * pFile = fopen( pFileName, "rb" );
    if ( pFile == NULL )
    {
        printf( "Cannot open file \"%s\" for reading.\n", pFileName );
        return NULL;
    }
    Abc_NamStrFindOrAdd( p, "module", NULL );
    assert( Abc_NamObjNumMax(p) == 2 );
    vTokens = Vec_IntAlloc( 1000 );
    while ( fgets( Buffer, MAX_LINE, pFile ) != NULL )
    {
        if ( Buffer[0] == '#' )
            continue;
        Rtl_TokenUnspace( Buffer );
        pTemp = strtok( Buffer, " \t\r\n" );
        if ( pTemp == NULL )
            continue;
        while ( pTemp )
        {
            if ( *pTemp == '\"' )  Rtl_TokenRespace( pTemp );
            Vec_IntPush( vTokens, Abc_NamStrFindOrAdd(p, pTemp, NULL) );
            pTemp = strtok( NULL, " \t\r\n" );
        }
        Vec_IntPush( vTokens, -1 );
    }
    fclose( pFile );
    return vTokens;
}




/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
extern void Rtl_NtkPrintSig( Rtl_Ntk_t * p, int Sig );

void Rtl_NtkPrintConst( Rtl_Ntk_t * p, int * pConst )
{
    int i;
    if ( pConst[0] == -1 )
    {
        fprintf( Rtl_NtkFile(p), " %d", pConst[1] );
        return;
    }
    fprintf( Rtl_NtkFile(p), " %d\'", pConst[0] );
    for ( i = pConst[0] - 1; i >= 0; i-- )
        fprintf( Rtl_NtkFile(p), "%d", Abc_InfoHasBit((unsigned *)pConst+1,i) );
}
void Rtl_NtkPrintSlice( Rtl_Ntk_t * p, int * pSlice )
{
    fprintf( Rtl_NtkFile(p), " %s", Rtl_NtkStr(p, pSlice[0]) );
    if ( pSlice[1] == pSlice[2] )
        fprintf( Rtl_NtkFile(p), " [%d]", pSlice[1] );
    else
        fprintf( Rtl_NtkFile(p), " [%d:%d]", pSlice[1], pSlice[2] );
}
void Rtl_NtkPrintConcat( Rtl_Ntk_t * p, int * pConcat )
{
    int i;
    fprintf( Rtl_NtkFile(p), " {" );
    for ( i = 1; i <= pConcat[0]; i++ )
        Rtl_NtkPrintSig( p, pConcat[i] );
    fprintf( Rtl_NtkFile(p), " }" );
}
void Rtl_NtkPrintSig( Rtl_Ntk_t * p, int Sig )
{
    if ( Rtl_SigIsNone(Sig) )
        fprintf( Rtl_NtkFile(p), " %s", Rtl_NtkStr(p, Sig >> 2) );
    else if ( Rtl_SigIsConst(Sig) )
        Rtl_NtkPrintConst( p, Vec_IntEntryP(&p->pLib->vConsts, Sig >> 2) );
    else if ( Rtl_SigIsSlice(Sig) )
        Rtl_NtkPrintSlice( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2) );
    else if ( Rtl_SigIsConcat(Sig) )
        Rtl_NtkPrintConcat( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2) );
    else assert( 0 );
}
void Rtl_NtkPrintWire( Rtl_Ntk_t * p, int * pWire )
{
    fprintf( Rtl_NtkFile(p), "  wire" );
    if ( pWire[1] != 1 )  fprintf( Rtl_NtkFile(p), " width %d",  pWire[1] );
    if ( pWire[2] != 0 )  fprintf( Rtl_NtkFile(p), " offset %d", pWire[2] );
    if ( pWire[0] & 8 )   fprintf( Rtl_NtkFile(p), " upto" );
    if ( pWire[0] & 1 )   fprintf( Rtl_NtkFile(p), " input %d",  pWire[3] );
    if ( pWire[0] & 2 )   fprintf( Rtl_NtkFile(p), " output %d", pWire[3] );
    if ( pWire[0] & 4 )   fprintf( Rtl_NtkFile(p), " signed" );
    fprintf( Rtl_NtkFile(p), " %s\n", Rtl_NtkStr(p, pWire[0] >> 4) );
}
void Rtl_NtkPrintCell( Rtl_Ntk_t * p, int * pCell )
{
    int i, Par, Val;
    Rtl_CellForEachAttr( p, pCell, Par, Val, i )
        fprintf( Rtl_NtkFile(p), "  attribute %s %s\n", Rtl_NtkStr(p, Par), Rtl_NtkStr(p, Val) );
        fprintf( Rtl_NtkFile(p), "  cell %s %s\n", Rtl_NtkStr(p, Rtl_CellType(pCell)), Rtl_NtkStr(p, pCell[1]) );
    Rtl_CellForEachParam( p, pCell, Par, Val, i )
        fprintf( Rtl_NtkFile(p), "    parameter" ), Rtl_NtkPrintSig(p, Par), Rtl_NtkPrintSig(p, Val), printf( "\n" );
    Rtl_CellForEachConnect( p, pCell, Par, Val, i )
        fprintf( Rtl_NtkFile(p), "    connect" ), Rtl_NtkPrintSig(p, Par), Rtl_NtkPrintSig(p, Val), printf( "\n" );
        fprintf( Rtl_NtkFile(p), "  end\n" );
}
void Rtl_NtkPrintConnection( Rtl_Ntk_t * p, int * pCon )
{
    fprintf( Rtl_NtkFile(p), "  connect" );
    Rtl_NtkPrintSig( p, pCon[0] );
    Rtl_NtkPrintSig( p, pCon[1] );
    fprintf( Rtl_NtkFile(p), "\n" );
}
void Rtl_NtkPrint( Rtl_Ntk_t * p )
{
    int i, Par, Val, * pWire, * pCell, * pCon;
    fprintf( Rtl_NtkFile(p), "\n" );
    Rtl_NtkForEachAttr( p, Par, Val, i )
        fprintf( Rtl_NtkFile(p), "attribute %s %s\n", Rtl_NtkStr(p, Par), Rtl_NtkStr(p, Val) );
    fprintf( Rtl_NtkFile(p), "module %s\n", Rtl_NtkName(p) );
    Rtl_NtkForEachWire( p, pWire, i )
        Rtl_NtkPrintWire( p, pWire );
    Rtl_NtkForEachCell( p, pCell, i )
        Rtl_NtkPrintCell( p, pCell );
    Rtl_NtkForEachCon( p, pCon, i )
        Rtl_NtkPrintConnection( p, pCon );
    fprintf( Rtl_NtkFile(p), "end\n" );
}
void Rtl_LibPrint( char * pFileName, Rtl_Lib_t * p )
{
    p->pFile = pFileName ? fopen( pFileName, "wb" ) : stdout;
    if ( p->pFile == NULL )
    {
        printf( "Cannot open output file \"%s\".\n", pFileName );
        return;
    }
    else
    {
        Rtl_Ntk_t * pNtk; int i;
        fprintf( p->pFile, "\n" );
        fprintf( p->pFile, "# Generated by ABC on %s\n", Extra_TimeStamp() );
        Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
            Rtl_NtkPrint( pNtk );    
        if ( p->pFile != stdout )
            fclose( p->pFile );
        p->pFile = NULL;
    }
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
extern int Rtl_NtkReadSig( Rtl_Ntk_t * p, int * pPos );

int Rtl_NtkReadConst( Rtl_Ntk_t * p, char * pConst )
{
    Vec_Int_t * vConst = &p->pLib->vConsts;
    int RetVal = Vec_IntSize( vConst );
    int Width  = atoi( pConst );
    assert( pConst[0] >= '0' && pConst[0] <= '9' );
    if ( strstr(pConst, "\'") )
    {
        int Length = strlen(pConst);
        int nWords = (Width + 31) / 32;
        int i, * pArray;
        Vec_IntPush( vConst, Width );
        Vec_IntFillExtra( vConst, Vec_IntSize(vConst) + nWords, 0 );
        pArray = Vec_IntEntryP( vConst, RetVal + 1 );
        for ( i = Length-1; i >= Length-Width; i-- )
            if ( pConst[i] == '1' )
                Abc_InfoSetBit( (unsigned *)pArray, Length-1-i );
    }
    else
    {
        Vec_IntPush( vConst, -1 );
        Vec_IntPush( vConst, Width );
    }
    return (RetVal << 2) | 1;
}
int Rtl_NtkReadSlice( Rtl_Ntk_t * p, char * pSlice, int NameId )
{
    Vec_Int_t * vSlice = &p->pLib->vSlices;
    int RetVal  = Vec_IntSize( vSlice );
    int Left    = atoi( pSlice+1 );
    char * pTwo = strstr( pSlice, ":" );
    int Right   = pTwo ? atoi( pTwo+1 ) : Left;
    assert( pSlice[0] == '[' && pSlice[strlen(pSlice)-1] == ']' );
    Vec_IntPush( vSlice, NameId );
    Vec_IntPush( vSlice, Left   );
    Vec_IntPush( vSlice, Right  );
    return (RetVal << 2) | 2;
}
int Rtl_NtkReadConcat( Rtl_Ntk_t * p, int * pPos )
{
    Vec_Int_t * vConcat = &p->pLib->vConcats;
    int RetVal = Vec_IntSize( vConcat ); char * pTok;
    Vec_IntPush( vConcat, ABC_INFINITY );
    do {
        int Sig = Rtl_NtkReadSig( p, pPos );
        Vec_IntPush( vConcat, Sig );
        pTok = Rtl_NtkTokStr( p, *pPos );
    } 
    while ( pTok[0] != '}' );
    Vec_IntWriteEntry( vConcat, RetVal, Vec_IntSize(vConcat) - RetVal - 1 );
    assert( pTok[0] == '}' );
    (*pPos)++;
    return (RetVal << 2) | 3;
}
int Rtl_NtkReadSig( Rtl_Ntk_t * p, int * pPos )
{
    int NameId  = Rtl_NtkTokId( p, *pPos );
    char * pSig = Rtl_NtkTokStr( p, (*pPos)++ );
    if ( pSig[0] >= '0' && pSig[0] <= '9' )
        return Rtl_NtkReadConst( p, pSig );
    if ( pSig[0] == '{' )
        return Rtl_NtkReadConcat( p, pPos );
    else 
    {
        char * pNext = Rtl_NtkTokStr( p, *pPos );
        if ( pNext && pNext[0] == '[' )
        {
            (*pPos)++;
            return Rtl_NtkReadSlice( p, pNext, NameId );
        }
        else
            return NameId << 2;
    }
}
int Rtl_NtkReadWire( Rtl_Ntk_t * p, int iPos )
{
    int i, Entry, Prev = -1;
    int Width = 1, Upto = 0, Offset = 0, Out = 0, In = 0, Number = 0, Signed = 0;
    assert( Rtl_NtkPosCheck(p, iPos-1, RTL_WIRE) );
    Vec_IntClear( &p->pLib->vAttrTemp );
    Vec_IntForEachEntryStart( p->pLib->vTokens, Entry, i, iPos )
    {
        //char * pTok = Rtl_NtkTokStr(p, i);
        if ( Entry == -1 )
            break;
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_WIDTH) )
            Width = atoi( Rtl_NtkTokStr(p, ++i) );
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_OFFSET) )
            Offset = atoi( Rtl_NtkTokStr(p, ++i) );
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_INPUT) )
            Number = atoi( Rtl_NtkTokStr(p, ++i) ), In = 1, p->nInputs++;
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_OUTPUT) )
            Number = atoi( Rtl_NtkTokStr(p, ++i) ), Out = 1, p->nOutputs++;
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_SIGNED) )
            Signed = 1;
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_UPTO) )
            Upto = 1;
        Prev = Entry;
    }
    // add WIRE_NUM=5 entries
    Vec_IntPush( &p->vWires, (Prev << 4) | (Upto << 3) | (Signed << 2) | (Out << 1) | (In << 0) );
    Vec_IntPush( &p->vWires, Width  );
    Vec_IntPush( &p->vWires, Offset );
    Vec_IntPush( &p->vWires, Number );
    Vec_IntPush( &p->vWires, -1 );
    assert( Rtl_NtkPosCheck(p, i, RTL_NONE) );
    return i;
}
int Rtl_NtkReadAttribute( Rtl_Ntk_t * p, int iPos )
{
//char * pTok1 = Rtl_NtkTokStr(p, iPos-1);
//char * pTok2 = Rtl_NtkTokStr(p, iPos);
//char * pTok3 = Rtl_NtkTokStr(p, iPos+1);
    assert( Rtl_NtkPosCheck(p, iPos-1, RTL_ATTRIBUTE) );
    Vec_IntPush( &p->pLib->vAttrTemp, Rtl_NtkTokId(p, iPos++) );
    Vec_IntPush( &p->pLib->vAttrTemp, Rtl_NtkTokId(p, iPos++) );
    assert( Rtl_NtkPosCheck(p, iPos, RTL_NONE) );
    return iPos;
}
int Rtl_NtkReadAttribute2( Rtl_Lib_t * p, int iPos )
{
//char * pTok1 = Abc_NamStr(p->pManName, Vec_IntEntry(p->vTokens, iPos-1));
//char * pTok2 = Abc_NamStr(p->pManName, Vec_IntEntry(p->vTokens, iPos)  );
//char * pTok3 = Abc_NamStr(p->pManName, Vec_IntEntry(p->vTokens, iPos+1));
    assert( Vec_IntEntry(p->vTokens, iPos-1) == p->pMap[RTL_ATTRIBUTE] );
    Vec_IntPush( &p->vAttrTemp, Vec_IntEntry(p->vTokens, iPos++) );
    Vec_IntPush( &p->vAttrTemp, Vec_IntEntry(p->vTokens, iPos++) );
    assert( Vec_IntEntry(p->vTokens, iPos) == p->pMap[RTL_NONE] );
    return iPos;
}
int Rtl_NtkReadConnect( Rtl_Ntk_t * p, int iPos )
{
//char * pTok1 = Rtl_NtkTokStr(p, iPos-1);
//char * pTok2 = Rtl_NtkTokStr(p, iPos);
//char * pTok3 = Rtl_NtkTokStr(p, iPos+1);
    assert( Rtl_NtkPosCheck(p, iPos-1, RTL_CONNECT) );
    Vec_IntPush( &p->vConns, Rtl_NtkReadSig(p, &iPos) );
    Vec_IntPush( &p->vConns, Rtl_NtkReadSig(p, &iPos) );
    assert( Rtl_NtkPosCheck(p, iPos, RTL_NONE) );
    return iPos;
}
int Rtl_NtkReadCell( Rtl_Ntk_t * p, int iPos )
{
    Vec_Int_t * vAttrs = &p->pLib->vAttrTemp;
    int iPosPars, iPosCons, Par, Val, i, Entry;
    assert( Rtl_NtkPosCheck(p, iPos-1, RTL_CELL) );
    Vec_IntPush( &p->vCells, Vec_IntSize(&p->vStore) );
    Vec_IntPush( &p->vStore, Rtl_NtkTokId(p, iPos++) ); // 0
    Vec_IntPush( &p->vStore, Rtl_NtkTokId(p, iPos++) ); // 1
    Vec_IntPush( &p->vStore, -1 );
    Vec_IntPush( &p->vStore, -1 );
    assert( Vec_IntSize(vAttrs) % 2 == 0 );
    Vec_IntPush( &p->vStore, Vec_IntSize(vAttrs)/2 );
    iPosPars = Vec_IntSize(&p->vStore);
    Vec_IntPush( &p->vStore, 0 );  // 5
    iPosCons = Vec_IntSize(&p->vStore);
    Vec_IntPush( &p->vStore, 0 );  // 6
    Vec_IntPush( &p->vStore, 0 );  // 7
    assert( Vec_IntSize(&p->vStore) == Vec_IntEntryLast(&p->vCells)+CELL_NUM );
    Vec_IntAppend( &p->vStore, vAttrs );
    Vec_IntClear( vAttrs );
    Vec_IntForEachEntryStart( p->pLib->vTokens, Entry, i, iPos )
    {
        if ( Rtl_NtkTokCheck(p, Entry, RTL_END) )
            break;
        if ( Rtl_NtkTokCheck(p, Entry, RTL_PARAMETER) || Rtl_NtkTokCheck(p, Entry, RTL_CONNECT) )
        {
            int iPosCount = Rtl_NtkTokCheck(p, Entry, RTL_PARAMETER) ? iPosPars : iPosCons;
            Vec_IntAddToEntry( &p->vStore, iPosCount, 1 );
            i++;
            Par = Rtl_NtkReadSig(p, &i);
            Val = Rtl_NtkReadSig(p, &i);            
            Vec_IntPushTwo( &p->vStore, Par, Val );
        }
        assert( Rtl_NtkPosCheck(p, i, RTL_NONE) );
    }
    assert( Rtl_NtkPosCheck(p, i, RTL_END) );
    i++;
    assert( Rtl_NtkPosCheck(p, i, RTL_NONE) );
    return i;
}
int Wln_ReadMatchEnd( Rtl_Ntk_t * p, int Mod )
{
    int i, Entry, Count = 0;
    Vec_IntForEachEntryStart( p->pLib->vTokens, Entry, i, Mod )
        if ( Rtl_NtkTokCheck(p, Entry, RTL_CELL) )
            Count++;
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_END) )
        {
            if ( Count == 0 )
                return i;
            Count--;
        }
    assert( 0 );
    return -1;
}
int Rtl_NtkReadNtk( Rtl_Lib_t * pLib, int Mod )
{
    Rtl_Ntk_t * p = Rtl_NtkAlloc( pLib );
    Vec_Int_t * vAttrs = &p->pLib->vAttrTemp;
    int End = Wln_ReadMatchEnd( p, Mod ), i, Entry;
    assert( Rtl_NtkPosCheck(p, Mod-1, RTL_MODULE) );
    assert( Rtl_NtkPosCheck(p, End, RTL_END)    );
    p->NameId = Rtl_NtkTokId( p, Mod );
    p->Slice0 = Vec_IntSize( &pLib->vSlices );
    Vec_IntAppend( &p->vAttrs, vAttrs );
    Vec_IntClear( vAttrs );
    Vec_IntForEachEntryStartStop( pLib->vTokens, Entry, i, Mod, End )
    {
        if ( Rtl_NtkTokCheck(p, Entry, RTL_WIRE) )
            i = Rtl_NtkReadWire( p, i+1 );
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_ATTRIBUTE) )
            i = Rtl_NtkReadAttribute( p, i+1 );
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_CELL) )
            i = Rtl_NtkReadCell( p, i+1 );
        else if ( Rtl_NtkTokCheck(p, Entry, RTL_CONNECT) )
            i = Rtl_NtkReadConnect( p, i+1 );
    }
    p->Slice1 = Vec_IntSize( &pLib->vSlices );
    assert( Vec_IntSize(&p->vWires) % WIRE_NUM == 0 );
    return End;
}
void Rtl_NtkReportUndefs( Rtl_Ntk_t * p )
{
    Vec_Int_t * vNames, * vCounts;
    int i, iName, * pCell;
    vNames  = Vec_IntAlloc( 10 );
    vCounts = Vec_IntAlloc( 10 );
    Rtl_NtkForEachCell( p, pCell, i )
        if ( Rtl_CellModule(pCell) == ABC_INFINITY-1 ) 
        {
            iName = Vec_IntFind(vNames, Rtl_CellType(pCell));
            if ( iName == -1 )
            {
                iName = Vec_IntSize(vNames);
                Vec_IntPush( vNames, Rtl_CellType(pCell) );
                Vec_IntPush( vCounts, 0 );
            }
            Vec_IntAddToEntry( vCounts, iName, 1 );
        }
    Vec_IntForEachEntry( vNames, iName, i )
        printf( "  %s (%d)", Rtl_NtkStr(p, iName), Vec_IntEntry(vCounts, i) );
    printf( "\n" );
    Vec_IntFree( vNames );
    Vec_IntFree( vCounts );
}
int Rtl_NtkSetParents( Rtl_Ntk_t * p )
{
    int i, * pCell, nUndef = 0;
    Rtl_NtkForEachCell( p, pCell, i )
    {
        pCell[2] = Rtl_NtkReadType( p, Rtl_CellType(pCell) );
        if ( Rtl_CellModule(pCell) == ABC_INFINITY-1 ) 
            nUndef++;
        else
            pCell[3] = Rtl_CellModule(pCell) < ABC_INFINITY ? pCell[6]-1 : Rtl_NtkModule(p, Rtl_CellModule(pCell)-ABC_INFINITY)->nInputs;
    }
    if ( !nUndef )
        return 0;
    printf( "Module \"%s\" has %d blackbox instances: ", Rtl_NtkName(p), nUndef );
    Rtl_NtkReportUndefs( p );
    return nUndef;
}
void Rtl_LibSetParents( Rtl_Lib_t * p )
{
    Rtl_Ntk_t * pNtk; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        Rtl_NtkSetParents( pNtk );  
}
void Rtl_LibReorderModules_rec( Rtl_Ntk_t * p, Vec_Ptr_t * vNew )
{
    int i, * pCell;
    Rtl_NtkForEachCell( p, pCell, i )
    {
        Rtl_Ntk_t * pMod = Rtl_CellNtk( p, pCell );
        if ( pMod && pMod->iCopy == -1 )
            Rtl_LibReorderModules_rec( pMod, vNew );
    }
    assert( p->iCopy == -1 );
    p->iCopy = Vec_PtrSize(vNew);
    Vec_PtrPush( vNew, p );
}
void Rtl_NtkUpdateBoxes( Rtl_Ntk_t * p )
{
    int i, * pCell;
    Rtl_NtkForEachCell( p, pCell, i )
    {
        Rtl_Ntk_t * pMod = Rtl_CellNtk( p, pCell );
        if ( pMod ) 
            pCell[2] = ABC_INFINITY + pMod->iCopy;
    }
}
void Rtl_LibUpdateBoxes( Rtl_Lib_t * p )
{
    Rtl_Ntk_t * pNtk; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        Rtl_NtkUpdateBoxes( pNtk );  
}
void Rtl_LibReorderModules( Rtl_Lib_t * p )
{
    Vec_Ptr_t * vNew = Vec_PtrAlloc( Vec_PtrSize(p->vNtks) );
    Rtl_Ntk_t * pNtk; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        pNtk->iCopy = -1;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, p->vNtks, pNtk, i )
        if ( pNtk->iCopy == -1 )
            Rtl_LibReorderModules_rec( pNtk, vNew );
    assert( Vec_PtrSize(p->vNtks) == Vec_PtrSize(vNew) );
    Rtl_LibUpdateBoxes( p );
    Vec_PtrClear( p->vNtks );
    Vec_PtrAppend( p->vNtks, vNew );
    Vec_PtrFree( vNew );
}
Rtl_Lib_t * Rtl_LibReadFile( char * pFileName, char * pFileSpec )
{
    Rtl_Lib_t * p = Rtl_LibAlloc(); int i, Entry;
    p->pSpec      = Abc_UtilStrsav( pFileSpec );
    p->pManName   = Abc_NamStart( 1000, 50 );
    p->vTokens    = Rtl_NtkReadFile( pFileName, p->pManName );
    Rtl_LibDeriveMap( p );
    Vec_IntClear( &p->vAttrTemp );
    Vec_IntForEachEntry( p->vTokens, Entry, i )
        if ( Entry == p->pMap[RTL_MODULE] )
            i = Rtl_NtkReadNtk( p, i+1 );
        else if ( Entry == p->pMap[RTL_ATTRIBUTE] )
            i = Rtl_NtkReadAttribute2( p, i+1 );
    Rtl_LibSetParents( p );
    Rtl_LibReorderModules( p );
    return p;
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
extern void Rtl_NtkCollectSignalRange( Rtl_Ntk_t * p, int Sig );

void Rtl_NtkCollectWireRange( Rtl_Ntk_t * p, int NameId, int Left, int Right )
{
    int Wire  = Rtl_WireMapNameToId( p, NameId );
    int First = Rtl_WireBitStart( p, Wire );
    int Width = Rtl_WireWidth( p, Wire ), i;
    Left  = Left  == -1 ? Width-1 :  Left;
    Right = Right == -1 ? 0       : Right;
    assert ( Right >= 0 && Right <= Left );
    for ( i = Right; i <= Left; i++ )
    {
        assert( Vec_IntEntry(&p->vLits, First+i) != -1 );
        Vec_IntPush( &p->vBitTemp, Vec_IntEntry(&p->vLits, First+i) );
    }
}
void Rtl_NtkCollectConstRange( Rtl_Ntk_t * p, int * pConst )
{
    int i, nLimit = pConst[0];
    if ( nLimit == -1 )
        nLimit = 32;
    //assert( pConst[0] > 0 );
    for ( i = 0; i < nLimit; i++ )
        Vec_IntPush( &p->vBitTemp, Abc_InfoHasBit((unsigned *)pConst+1,i) );
}
void Rtl_NtkCollectSliceRange( Rtl_Ntk_t * p, int * pSlice )
{
    Rtl_NtkCollectWireRange( p, pSlice[0], pSlice[1], pSlice[2] );
}
void Rtl_NtkCollectConcatRange( Rtl_Ntk_t * p, int * pConcat )
{
    int i;
    for ( i = pConcat[0]; i >= 1; i-- )
        Rtl_NtkCollectSignalRange( p, pConcat[i] );
}
void Rtl_NtkCollectSignalRange( Rtl_Ntk_t * p, int Sig )
{
    if ( Rtl_SigIsNone(Sig) )
        Rtl_NtkCollectWireRange( p, Sig >> 2, -1, -1 );
    else if ( Rtl_SigIsConst(Sig) )
        Rtl_NtkCollectConstRange( p, Vec_IntEntryP(&p->pLib->vConsts, Sig >> 2) );
    else if ( Rtl_SigIsSlice(Sig) )
        Rtl_NtkCollectSliceRange( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2) );
    else if ( Rtl_SigIsConcat(Sig) )
        Rtl_NtkCollectConcatRange( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2) );
    else assert( 0 );
}


extern int Rtl_NtkInsertSignalRange( Rtl_Ntk_t * p, int Sig, int * pLits, int nLits );

int Rtl_NtkInsertWireRange( Rtl_Ntk_t * p, int NameId, int Left, int Right, int * pLits, int nLits )
{
    //char * pName = Rtl_NtkStr( p, NameId );
    int Wire  = Rtl_WireMapNameToId( p, NameId );
    int First = Rtl_WireBitStart( p, Wire );
    int Width = Rtl_WireWidth( p, Wire ), i, k = 0;
    Left  = Left  == -1 ? Width-1 :  Left;
    Right = Right == -1 ? 0       : Right;
    assert ( Right >= 0 && Right <= Left );
    for ( i = Right; i <= Left; i++ )
    {
        assert( Vec_IntEntry(&p->vLits, First+i) == -1 );
        Vec_IntWriteEntry(&p->vLits, First+i, pLits[k++] );
    }
    assert( k <= nLits );
    return k;
}
int Rtl_NtkInsertSliceRange( Rtl_Ntk_t * p, int * pSlice, int * pLits, int nLits )
{
    return Rtl_NtkInsertWireRange( p, pSlice[0], pSlice[1], pSlice[2], pLits, nLits );
}
int Rtl_NtkInsertConcatRange( Rtl_Ntk_t * p, int * pConcat, int * pLits, int nLits )
{
    int i, k = 0;
    for ( i = 1; i <= pConcat[0]; i++ )
        k += Rtl_NtkInsertSignalRange( p, pConcat[i], pLits+k, nLits-k );
    assert( k <= nLits );
    return k;
}
int Rtl_NtkInsertSignalRange( Rtl_Ntk_t * p, int Sig, int * pLits, int nLits )
{
    int nBits = ABC_INFINITY;
    if ( Rtl_SigIsNone(Sig) )
        nBits = Rtl_NtkInsertWireRange( p, Sig >> 2, -1, -1, pLits, nLits );
    if ( Rtl_SigIsSlice(Sig) )
        nBits = Rtl_NtkInsertSliceRange( p, Vec_IntEntryP(&p->pLib->vSlices, Sig >> 2), pLits, nLits );
    if ( Rtl_SigIsConcat(Sig) )
        nBits = Rtl_NtkInsertConcatRange( p, Vec_IntEntryP(&p->pLib->vConcats, Sig >> 2), pLits, nLits );
    if ( Rtl_SigIsConst(Sig) )
        assert( 0 );
    assert( nBits == nLits );
    return nBits;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rtl_NtkBlastInputs( Gia_Man_t * pNew, Rtl_Ntk_t * p )
{
    int b, i;
    for ( i = 0; i < p->nInputs; i++ )
    {
        int First = Rtl_WireBitStart( p, i );
        int Width = Rtl_WireWidth( p, i );
        for ( b = 0; b < Width; b++ )
        {
            assert( Vec_IntEntry(&p->vLits, First+b) == -1 );
            Vec_IntWriteEntry( &p->vLits, First+b, Gia_ManAppendCi(pNew) );
        }
    }
}
void Rtl_NtkBlastOutputs( Gia_Man_t * pNew, Rtl_Ntk_t * p )
{
    int b, i;
    for ( i = 0; i < p->nOutputs; i++ )
    {
        int First = Rtl_WireBitStart( p, p->nInputs + i );
        int Width = Rtl_WireWidth( p, p->nInputs + i );
        for ( b = 0; b < Width; b++ )
        {
            assert( Vec_IntEntry(&p->vLits, First+b) != -1 );
            Gia_ManAppendCo( pNew, Vec_IntEntry(&p->vLits, First+b) );
        }
    }
}
void Rtl_NtkBlastConnect( Gia_Man_t * pNew, Rtl_Ntk_t * p, int * pCon )
{
    int nBits;
    Vec_IntClear( &p->vBitTemp );
    Rtl_NtkCollectSignalRange( p, pCon[0] );
    nBits = Rtl_NtkInsertSignalRange( p, pCon[1], Vec_IntArray(&p->vBitTemp), Vec_IntSize(&p->vBitTemp) );
    assert( nBits == Vec_IntSize(&p->vBitTemp) );
    //printf( "Finished blasting connection (Value = %d).\n", Vec_IntEntry(&p->vBitTemp, 0) );
}
void Rtl_NtkBlastHierarchy( Gia_Man_t * pNew, Rtl_Ntk_t * p, int * pCell )
{
    extern Gia_Man_t * Rtl_NtkBlast( Rtl_Ntk_t * p );
    extern void Gia_ManDupRebuild( Gia_Man_t * pNew, Gia_Man_t * p, Vec_Int_t * vLits );
    Rtl_Ntk_t * pModel = Rtl_NtkModule( p, Rtl_CellModule(pCell)-ABC_INFINITY );
    int k, Par, Val, nBits = 0;
    Vec_IntClear( &p->vBitTemp );
    Rtl_CellForEachInput( p, pCell, Par, Val, k )
        Rtl_NtkCollectSignalRange( p, Val );
//    if ( pModel->pGia == NULL )
//        pModel->pGia = Rtl_NtkBlast( pModel );
    assert( pModel->pGia );
    Gia_ManDupRebuild( pNew, pModel->pGia, &p->vBitTemp );
    Rtl_CellForEachOutput( p, pCell, Par, Val, k )
        nBits += Rtl_NtkInsertSignalRange( p, Val, Vec_IntArray(&p->vBitTemp)+nBits, Vec_IntSize(&p->vBitTemp)-nBits );
    assert( nBits == Vec_IntSize(&p->vBitTemp) );
}

int Rtl_NtkCellParamValue( Rtl_Ntk_t * p, int * pCell, char * pParam )
{
    int ParamId = Rtl_NtkStrId( p, pParam );
    int i, Par, Val, ValOut = ABC_INFINITY, * pConst;
//    p->pLib->pFile = stdout;
//    Rtl_CellForEachParam( p, pCell, Par, Val, i )
//        fprintf( Rtl_NtkFile(p), "    parameter" ), Rtl_NtkPrintSig(p, Par), Rtl_NtkPrintSig(p, Val), printf( "\n" );
    Rtl_CellForEachParam( p, pCell, Par, Val, i )
        if ( (Par >> 2) == ParamId )
        {
            assert( Rtl_SigIsConst(Val) );
            pConst = Vec_IntEntryP( &p->pLib->vConsts, Val >> 2 );
            assert( pConst[0] < 32 );
            ValOut = pConst[1];
        }
    return ValOut;
}
void Rtl_NtkBlastOperator( Gia_Man_t * pNew, Rtl_Ntk_t * p, int * pCell )
{
    extern void Rtl_NtkBlastNode( Gia_Man_t * pNew, int Type, int nIns, Vec_Int_t * vDatas, int nRange, int fSign0, int fSign1 );
    Vec_Int_t * vRes = &p->pLib->vTemp[3];
    int i, Par, Val, ValOut = -1, nBits = 0, nRange = -1;
    int fSign0 = Rtl_NtkCellParamValue( p, pCell, "\\A_SIGNED" );
    int fSign1 = Rtl_NtkCellParamValue( p, pCell, "\\B_SIGNED" );
    Rtl_CellForEachOutput( p, pCell, Par, ValOut, i )
        nRange = Rtl_NtkCountSignalRange( p, ValOut );
    assert( nRange > 0 );
    for ( i = 0; i < TEMP_NUM; i++ )
        Vec_IntClear( &p->pLib->vTemp[i] );
    //printf( "Starting blasting cell %s.\n", Rtl_CellNameStr(p, pCell) );
    Rtl_CellForEachInput( p, pCell, Par, Val, i )
    {
        Vec_IntClear( &p->vBitTemp );
        Rtl_NtkCollectSignalRange( p, Val );
        Vec_IntAppend( &p->pLib->vTemp[i], &p->vBitTemp );
    }
    Rtl_NtkBlastNode( pNew, Rtl_CellModule(pCell), Rtl_CellInputNum(pCell), p->pLib->vTemp, nRange, fSign0, fSign1 );
    assert( Vec_IntSize(vRes) > 0 );
    nBits = Rtl_NtkInsertSignalRange( p, ValOut, Vec_IntArray(vRes)+nBits, Vec_IntSize(vRes)-nBits );
    assert( nBits == Vec_IntSize(vRes) );
    //printf( "Finished blasting cell %s (Value = %d).\n", Rtl_CellNameStr(p, pCell), Vec_IntEntry(vRes, 0) );
}
Gia_Man_t * Rtl_NtkBlast( Rtl_Ntk_t * p )
{
    Gia_Man_t * pTemp, * pNew = Gia_ManStart( 1000 );
    int i, iObj, * pCell, nBits = Rtl_NtkRangeWires( p );
    char Buffer[100]; static int counter = 0;
    Vec_IntFill( &p->vLits, nBits, -1 );
    Rtl_NtkMapWires( p, 0 );
    Rtl_NtkBlastInputs( pNew, p );
    Gia_ManHashAlloc( pNew );
    Vec_IntForEachEntry( &p->vOrder, iObj, i )
    {
        iObj -= Rtl_NtkInputNum(p);
        if ( iObj < 0 )
            continue;
        if ( iObj >= Rtl_NtkCellNum(p) )
        {
            Rtl_NtkBlastConnect( pNew, p, Rtl_NtkCon(p, iObj - Rtl_NtkCellNum(p)) );
            continue;
        }
        pCell = Rtl_NtkCell(p, iObj);
        if ( Rtl_CellModule(pCell) >= ABC_INFINITY )
            Rtl_NtkBlastHierarchy( pNew, p, pCell );
        else if ( Rtl_CellModule(pCell) < ABC_OPER_LAST )
            Rtl_NtkBlastOperator( pNew, p, pCell );
        else
            printf( "Cannot blast black box %s in module %s.\n", Rtl_NtkStr(p, Rtl_CellType(pCell)), Rtl_NtkName(p) );
    }
    Gia_ManHashStop( pNew );
    Rtl_NtkBlastOutputs( pNew, p );
    Rtl_NtkMapWires( p, 1 );
    pNew = Gia_ManCleanup( pTemp = pNew );
    Gia_ManStop( pTemp );

sprintf( Buffer, "temp%d.aig", counter++ );
//sprintf( Buffer, "temp%d.aig", counter );
Gia_AigerWrite( pNew, Buffer, 0, 0, 0 );
printf( "Dumpted blasted AIG into file \"%s\" for module \"%s\".\n", Buffer, Rtl_NtkName(p) );
Gia_ManPrintStats( pNew, NULL );
    return pNew;
}
void Rtl_LibBlast( Rtl_Lib_t * pLib )
{
    Rtl_Ntk_t * p; int i;
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        if ( p->pGia == NULL )
            p->pGia = Rtl_NtkBlast( p );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rtl_LibPreprocess( Rtl_Lib_t * pLib )
{
    abctime clk = Abc_Clock(); 
    Rtl_Ntk_t * p1 = NULL, * p2 = NULL, * p;
    int i, k, Status, fFound = 0;
    printf( "Performing preprocessing for verification.\n" );
    // find similar modules
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p1, i )
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p2, k )
    {
        if ( i >= k )  
            continue;
        if ( Gia_ManCiNum(p1->pGia) != Gia_ManCiNum(p2->pGia) || 
             Gia_ManCoNum(p1->pGia) != Gia_ManCoNum(p2->pGia) )
            continue;
        // two similar modules
        Status = Cec_ManVerifyTwo( p1->pGia, p2->pGia, 0 );
        if ( Status != 1 )
            continue;
        printf( "Proved equivalent modules: %s == %s\n", Rtl_NtkName(p1), Rtl_NtkName(p2) );
        // inline
        if ( Gia_ManAndNum(p1->pGia) > Gia_ManAndNum(p2->pGia) )
            ABC_SWAP( Gia_Man_t *, p1->pGia, p2->pGia );
        assert( Gia_ManAndNum(p1->pGia) <= Gia_ManAndNum(p2->pGia) );
        Gia_ManStopP( &p2->pGia );
        p2->pGia = Gia_ManDup( p1->pGia );
        fFound = 1;
        goto finish;
    }
finish:
    if ( fFound == 0 )
        printf( "Preprocessing not succeded.\n" );
    Abc_PrintTime( 1, "Preprocessing time", Abc_Clock() - clk );
    // blast AIGs again
    Vec_PtrForEachEntry( Rtl_Ntk_t *, pLib->vNtks, p, i )
        if ( p != p1 && p != p2 )
            Gia_ManStopP( &p->pGia );
    Rtl_LibBlast( pLib );
}
void Rtl_LibSolve( Rtl_Lib_t * pLib )
{
    abctime clk = Abc_Clock(); int Status;
    Rtl_Ntk_t * pTop = Rtl_LibTop( pLib );
    Gia_Man_t * pCopy = Gia_ManDup( pTop->pGia );
    Gia_ManInvertPos( pCopy );
    Gia_ManAppendCo( pCopy, 0 );
    Status = Cec_ManVerifySimple( pCopy );
    Gia_ManStop( pCopy );
    if ( Status == 1 )
        printf( "Verification problem solved!  " );
    else
        printf( "Verification problem is NOT solved!  " );
    Abc_PrintTime( 1, "Time", Abc_Clock() - clk );
}


////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

