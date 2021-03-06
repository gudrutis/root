/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 *    File: $Id: RooListProxy.h,v 1.11 2007/07/13 21:24:36 wouter Exp $
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *                                                                           *
 * Copyright (c) 2000-2005, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/
#ifndef ROO_LIST_PROXY
#define ROO_LIST_PROXY

#include "TObject.h"
#include "RooAbsProxy.h"
#include "RooLinkedListIter.h"
#include "RooAbsArg.h"
#include "RooArgList.h"

class RooListProxy : public RooArgList, public RooAbsProxy  {
public:

  // Constructors, assignment etc.
  RooListProxy() : _defValueServer(kTRUE), _defShapeServer(kFALSE) { _owner=0 ; } ;
  RooListProxy(const char* name, const char* desc, RooAbsArg* owner, 
	      Bool_t defValueServer=kTRUE, Bool_t defShapeServer=kFALSE) ;
  RooListProxy(const char* name, RooAbsArg* owner, const RooListProxy& other) ;
  virtual ~RooListProxy() ;

  virtual const char* name() const { return GetName() ; }

  // List content management (modified for server hooks)
  virtual Bool_t add(const RooAbsArg& var, Bool_t silent=kFALSE) ;
  virtual Bool_t add(const RooAbsCollection& list, Bool_t silent=kFALSE) { return RooAbsCollection::add(list,silent) ; }
  virtual Bool_t add(const RooAbsArg& var, Bool_t valueServer, Bool_t shapeServer, Bool_t silent) ;
  virtual Bool_t addOwned(RooAbsArg& var, Bool_t silent=kFALSE);
  virtual Bool_t addOwned(const RooAbsCollection& list, Bool_t silent=kFALSE) { return RooAbsCollection::addOwned(list,silent) ; }
  virtual Bool_t replace(const RooAbsArg& var1, const RooAbsArg& var2) ;
  virtual Bool_t remove(const RooAbsArg& var, Bool_t silent=kFALSE, Bool_t matchByNameOnly=kFALSE) ;
  virtual void removeAll() ;

  RooListProxy& operator=(const RooArgList& other) ;

  virtual void print(std::ostream& os, Bool_t addContents=kFALSE) const ;
  
protected:
    
  RooAbsArg* _owner ;       // Pointer to owner
  Bool_t _defValueServer ;  // Propagate value dirty flags?
  Bool_t _defShapeServer ;  // Propagate shape dirty flags?

  virtual Bool_t changePointer(const RooAbsCollection& newServerSet, Bool_t nameChange=kFALSE, Bool_t factoryInitMode=kFALSE) ;

  ClassDef(RooListProxy,1) // Proxy class for a RooArgList
};

#endif

