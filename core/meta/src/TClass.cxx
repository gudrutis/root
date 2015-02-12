// @(#)root/meta:$Id: 7109cb45f1219c2aae6be19906ae5a63e31972ef $
// Author: Rene Brun   07/01/95

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
//  The ROOT global object gROOT contains a list of all defined         //
//  classes. This list is build when a reference to a class dictionary  //
//  is made. When this happens, the static "class"::Dictionary()        //
//  function is called to create a TClass object describing the         //
//  class. The Dictionary() function is defined in the ClassDef         //
//  macro and stored (at program startup or library load time) together //
//  with the class name in the TClassTable singleton object.            //
//  For a description of all dictionary classes see TDictionary.        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//*-*x7.5 macros/layout_class

#include "TClass.h"

#include "Riostream.h"
#include "TBaseClass.h"
#include "TBrowser.h"
#include "TBuffer.h"
#include "TClassGenerator.h"
#include "TClassEdit.h"
#include "TClassMenuItem.h"
#include "TClassRef.h"
#include "TClassTable.h"
#include "TDataMember.h"
#include "TDataType.h"
#include "TEnum.h" 
#include "TError.h"
#include "TExMap.h"
#include "TFunctionTemplate.h"
#include "THashList.h"
#include "TInterpreter.h"
#include "TMemberInspector.h"
#include "TMethod.h"
#include "TMethodArg.h"
#include "TMethodCall.h"
#include "TObjArray.h"
#include "TProtoClass.h"
#include "TROOT.h"
#include "TRealData.h"
#include "TStreamer.h"
#include "TStreamerElement.h"
#include "TVirtualStreamerInfo.h"
#include "TVirtualCollectionProxy.h"
#include "TVirtualIsAProxy.h"
#include "TVirtualRefProxy.h"
#include "TVirtualMutex.h"
#include "TVirtualPad.h"
#include "THashTable.h"
#include "TSchemaRuleSet.h"
#include "TGenericClassInfo.h"
#include "TIsAProxy.h"
#include "TSchemaRule.h"
#include "TSystem.h"
#include "TThreadSlots.h"

#include <cstdio>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <map>
#include <typeinfo>
#include <cmath>
#include <assert.h>
#include <vector>

#include "TListOfDataMembers.h"
#include "TListOfFunctions.h"
#include "TListOfFunctionTemplates.h"
#include "TListOfEnums.h"
#include "TViewPubDataMembers.h"
#include "TViewPubFunctions.h"

using namespace std;

// Mutex to protect CINT and META operations
// (exported to be used for similar cases in related classes)

TVirtualMutex* gInterpreterMutex = 0;

void *gMmallocDesc = 0; //is used and set in TMapFile
namespace {
   class TMmallocDescTemp {
   private:
      void *fSave;
   public:
      TMmallocDescTemp(void *value = 0) : fSave(gMmallocDesc) { gMmallocDesc = value; }
      ~TMmallocDescTemp() { gMmallocDesc = fSave; }
   };
}

std::atomic<Int_t> TClass::fgClassCount;

// Implementation of the TDeclNameRegistry

//______________________________________________________________________________
TClass::TDeclNameRegistry::TDeclNameRegistry(Int_t verbLevel): fVerbLevel(verbLevel){}

//______________________________________________________________________________
void TClass::TDeclNameRegistry::AddQualifiedName(const char *name)
{
   // Extract this part of the name
   // 1) Templates ns::ns2::,,,::THISPART<...
   // 2) Namespaces,classes ns::ns2::,,,::THISPART

   // Sanity check
   auto strLen = strlen(name);
   if (strLen == 0) return;
   // find <. If none, put end of string
   const char* endCharPtr = strchr(name, '<');
   endCharPtr = !endCharPtr ? &name[strLen] : endCharPtr;
   // find last : before the <. If not found, put begin of string
   const char* beginCharPtr = endCharPtr;
   while (beginCharPtr!=name){
      if (*beginCharPtr==':'){
         beginCharPtr++;
         break;
      }
      beginCharPtr--;
   }
   beginCharPtr = beginCharPtr!=endCharPtr ? beginCharPtr : name;
   std::string s(beginCharPtr, endCharPtr);
   if (fVerbLevel>1)
      printf("TDeclNameRegistry::AddQualifiedName Adding key %s for class/namespace %s\n", s.c_str(), name);
   TClass::TSpinLockGuard slg(fSpinLock);
   fClassNamesSet.insert(s);
}

//______________________________________________________________________________
Bool_t TClass::TDeclNameRegistry::HasDeclName(const char *name) const
{
   Bool_t found = false;
   {
      TClass::TSpinLockGuard slg(fSpinLock);
      found = fClassNamesSet.find(name) != fClassNamesSet.end();
   }
   return found;
}

//______________________________________________________________________________
TClass::TDeclNameRegistry::~TDeclNameRegistry()
{
   if (fVerbLevel>1){
      printf("TDeclNameRegistry Destructor. List of names:\n");
      for(auto const & key:fClassNamesSet){
         printf(" - %s\n",key.c_str());
      }
   }
}

// Implementation of the spinlock guard in the registry

//______________________________________________________________________________
TClass::TSpinLockGuard::TSpinLockGuard(std::atomic_flag& aflag):fAFlag(aflag)
{
   while (fAFlag.test_and_set(std::memory_order_acquire));
}

//______________________________________________________________________________
TClass::TSpinLockGuard::~TSpinLockGuard()
{
   fAFlag.clear(std::memory_order_release);
}

//______________________________________________________________________________
TClass::InsertTClassInRegistryRAII::InsertTClassInRegistryRAII(TClass::EState &state,
                                   const char *name,
                                   TDeclNameRegistry &emuRegistry): fState(state),fName(name), fNoInfoOrEmuOrFwdDeclNameRegistry(emuRegistry) {}

//______________________________________________________________________________
TClass::InsertTClassInRegistryRAII::~InsertTClassInRegistryRAII() {
   if (fState == TClass::kNoInfo ||
       fState == TClass::kEmulated ||
       fState == TClass::kForwardDeclared){
      fNoInfoOrEmuOrFwdDeclNameRegistry.AddQualifiedName(fName);
      }
   }

// In itialise the global member of TClass
TClass::TDeclNameRegistry TClass::fNoInfoOrEmuOrFwdDeclNameRegistry;

//Intent of why/how TClass::New() is called
//[Not a static datamember because MacOS does not support static thread local data member ... who knows why]
TClass::ENewType &TClass__GetCallingNew() {
   TTHREAD_TLS(TClass::ENewType) fgCallingNew = TClass::kRealNew;
   return fgCallingNew;
}

struct ObjRepoValue {
   ObjRepoValue(const TClass *what, Version_t version) : fClass(what),fVersion(version) {}
   const TClass *fClass;
   Version_t     fVersion;
};

static TVirtualMutex* gOVRMutex = 0;
typedef std::multimap<void*, ObjRepoValue> RepoCont_t;
static RepoCont_t gObjectVersionRepository;

static void RegisterAddressInRepository(const char * /*where*/, void *location, const TClass *what)
{
   // Register the object for special handling in the destructor.

   Version_t version = what->GetClassVersion();
//    if (!gObjectVersionRepository.count(location)) {
//       Info(where, "Registering address %p of class '%s' version %d", location, what->GetName(), version);
//    } else {
//       Warning(where, "Registering address %p again of class '%s' version %d", location, what->GetName(), version);
//    }
   {
      R__LOCKGUARD2(gOVRMutex);
      gObjectVersionRepository.insert(RepoCont_t::value_type(location, RepoCont_t::mapped_type(what,version)));
   }
#if 0
   // This code could be used to prevent an address to be registered twice.
   std::pair<RepoCont_t::iterator, Bool_t> tmp = gObjectVersionRepository.insert(RepoCont_t::value_type>(location, RepoCont_t::mapped_type(what,version)));
   if (!tmp.second) {
      Warning(where, "Reregistering an object of class '%s' version %d at address %p", what->GetName(), version, p);
      gObjectVersionRepository.erase(tmp.first);
      tmp = gObjectVersionRepository.insert(RepoCont_t::value_type>(location, RepoCont_t::mapped_type(what,version)));
      if (!tmp.second) {
         Warning(where, "Failed to reregister an object of class '%s' version %d at address %p", what->GetName(), version, location);
      }
   }
#endif
}

static void UnregisterAddressInRepository(const char * /*where*/, void *location, const TClass *what)
{
   // Remove an address from the repository of address/object.

   R__LOCKGUARD2(gOVRMutex);
   RepoCont_t::iterator cur = gObjectVersionRepository.find(location);
   for (; cur != gObjectVersionRepository.end();) {
      RepoCont_t::iterator tmp = cur++;
      if ((tmp->first == location) && (tmp->second.fVersion == what->GetClassVersion())) {
         // -- We still have an address, version match.
         // Info(where, "Unregistering address %p of class '%s' version %d", location, what->GetName(), what->GetClassVersion());
         gObjectVersionRepository.erase(tmp);
      } else {
         // -- No address, version match, we've reached the end.
         break;
      }
   }
}

static void MoveAddressInRepository(const char * /*where*/, void *oldadd, void *newadd, const TClass *what)
{
   // Register in the repository that an object has moved.

   // Move not only the object itself but also any base classes or sub-objects.
   size_t objsize = what->Size();
   long delta = (char*)newadd - (char*)oldadd;
   R__LOCKGUARD2(gOVRMutex);
   RepoCont_t::iterator cur = gObjectVersionRepository.find(oldadd);
   for (; cur != gObjectVersionRepository.end();) {
      RepoCont_t::iterator tmp = cur++;
      if (oldadd <= tmp->first && tmp->first < ( ((char*)oldadd) + objsize) ) {
         // The location is within the object, let's move it.

         gObjectVersionRepository.insert(RepoCont_t::value_type(((char*)tmp->first)+delta, RepoCont_t::mapped_type(tmp->second.fClass,tmp->second.fVersion)));
         gObjectVersionRepository.erase(tmp);

      } else {
         // -- No address, version match, we've reached the end.
         break;
      }
   }
}

//______________________________________________________________________________
//______________________________________________________________________________
namespace ROOT {
#define R__USE_STD_MAP
   class TMapTypeToTClass {
#if defined R__USE_STD_MAP
     // This wrapper class allow to avoid putting #include <map> in the
     // TROOT.h header file.
   public:
#ifdef R__GLOBALSTL
      typedef map<string,TClass*>                 IdMap_t;
#else
      typedef std::map<std::string,TClass*>       IdMap_t;
#endif
      typedef IdMap_t::key_type                   key_type;
      typedef IdMap_t::const_iterator             const_iterator;
      typedef IdMap_t::size_type                  size_type;
#ifdef R__WIN32
     // Window's std::map does NOT defined mapped_type
      typedef TClass*                             mapped_type;
#else
      typedef IdMap_t::mapped_type                mapped_type;
#endif

   private:
      IdMap_t fMap;

   public:
      void Add(const key_type &key, mapped_type &obj)
      {
         // Add the <key,obj> pair to the map.
         fMap[key] = obj;
      }
      mapped_type Find(const key_type &key) const
      {
         // Find the type corresponding to the key.
         IdMap_t::const_iterator iter = fMap.find(key);
         mapped_type cl = 0;
         if (iter != fMap.end()) cl = iter->second;
         return cl;
      }
      void Remove(const key_type &key) {
         // Remove the type corresponding to the key.
         fMap.erase(key);
      }
#else
   private:
      TMap fMap;

   public:
#ifdef R__COMPLETE_MEM_TERMINATION
      TMapTypeToTClass() {
         TIter next(&fMap);
         TObjString *key;
         while((key = (TObjString*)next())) {
            delete key;
         }
      }
#endif
      void Add(const char *key, TClass *&obj) {
         TObjString *realkey = new TObjString(key);
         fMap.Add(realkey, obj);
      }
      TClass* Find(const char *key) const {
         const TPair *a = (const TPair *)fMap.FindObject(key);
         if (a) return (TClass*) a->Value();
         return 0;
      }
      void Remove(const char *key) {
         TObjString realkey(key);
         TObject *actual = fMap.Remove(&realkey);
         delete actual;
      }
#endif
   };

   class TMapDeclIdToTClass {
   // Wrapper class for the multimap of DeclId_t and TClass.
   public:
      typedef multimap<TDictionary::DeclId_t, TClass*>   DeclIdMap_t;
      typedef DeclIdMap_t::key_type                      key_type;
      typedef DeclIdMap_t::mapped_type                   mapped_type;
      typedef DeclIdMap_t::const_iterator                const_iterator;
      typedef std::pair <const_iterator, const_iterator> equal_range;
      typedef DeclIdMap_t::size_type                     size_type;

   private:
      DeclIdMap_t fMap;

   public:
      void Add(const key_type &key, mapped_type obj)
      {
         // Add the <key,obj> pair to the map.
         std::pair<const key_type, mapped_type> pair = make_pair(key, obj);
         fMap.insert(pair);
      }
      size_type CountElementsWithKey(const key_type &key)
      {
         return fMap.count(key);
      }
      equal_range Find(const key_type &key) const
      {
         // Find the type corresponding to the key.
         return fMap.equal_range(key);
      }
      void Remove(const key_type &key) {
         // Remove the type corresponding to the key.
         fMap.erase(key);
      }
   };
}

IdMap_t *TClass::GetIdMap() {

#ifdef R__COMPLETE_MEM_TERMINATION
   static IdMap_t gIdMapObject;
   return &gIdMap;
#else
   static IdMap_t *gIdMap = new IdMap_t;
   return gIdMap;
#endif
}

DeclIdMap_t *TClass::GetDeclIdMap() {

#ifdef R__COMPLETE_MEM_TERMINATION
   static DeclIdMap_t gDeclIdMapObject;
   return &gIdMap;
#else
   static DeclIdMap_t *gDeclIdMap = new DeclIdMap_t;
   return gDeclIdMap;
#endif
}

//______________________________________________________________________________
void TClass::AddClass(TClass *cl)
{
   // static: Add a class to the list and map of classes.

   if (!cl) return;

   R__LOCKGUARD2(gInterpreterMutex);
   gROOT->GetListOfClasses()->Add(cl);
   if (cl->GetTypeInfo()) {
      GetIdMap()->Add(cl->GetTypeInfo()->name(),cl);
   }
   if (cl->fClassInfo) {
      GetDeclIdMap()->Add((void*)(cl->fClassInfo), cl);
   }
}

//______________________________________________________________________________
void TClass::AddClassToDeclIdMap(TDictionary::DeclId_t id, TClass* cl)
{
   // static: Add a TClass* to the map of classes.

   if (!cl || !id) return;
   GetDeclIdMap()->Add(id, cl);
}

//______________________________________________________________________________
void TClass::RemoveClass(TClass *oldcl)
{
   // static: Remove a class from the list and map of classes

   if (!oldcl) return;

   R__LOCKGUARD2(gInterpreterMutex);
   gROOT->GetListOfClasses()->Remove(oldcl);
   if (oldcl->GetTypeInfo()) {
      GetIdMap()->Remove(oldcl->GetTypeInfo()->name());
   }
   if (oldcl->fClassInfo) {
      //GetDeclIdMap()->Remove((void*)(oldcl->fClassInfo));
   }
}

//______________________________________________________________________________
void TClass::RemoveClassDeclId(TDictionary::DeclId_t id)
{
   if (!id) return;
   GetDeclIdMap()->Remove(id);
}

//______________________________________________________________________________
void ROOT::Class_ShowMembers(TClass *cl, const void *obj, TMemberInspector&insp)
{
   // Indirect call to the implementation of ShowMember allowing [forward]
   // declaration with out a full definition of the TClass class.

   gInterpreter->InspectMembers(insp, obj, cl, kFALSE);
}

//______________________________________________________________________________
//______________________________________________________________________________

class TDumpMembers : public TMemberInspector {
   bool fNoAddr;
public:
   TDumpMembers(bool noAddr): fNoAddr(noAddr) { }

   using TMemberInspector::Inspect;
   void Inspect(TClass *cl, const char *parent, const char *name, const void *addr, Bool_t isTransient);
};

//______________________________________________________________________________
void TDumpMembers::Inspect(TClass *cl, const char *pname, const char *mname, const void *add, Bool_t /* isTransient */)
{
   // Print value of member mname.
   //
   // This method is called by the ShowMembers() method for each
   // data member when object.Dump() is invoked.
   //
   //    cl    is the pointer to the current class
   //    pname is the parent name (in case of composed objects)
   //    mname is the data member name
   //    add   is the data member address

   const Int_t kvalue = 30;
#ifdef R__B64
   const Int_t ktitle = 50;
#else
   const Int_t ktitle = 42;
#endif
   const Int_t kline  = 1024;
   Int_t cdate = 0;
   Int_t ctime = 0;
   UInt_t *cdatime = 0;
   char line[kline];

   TDataType *membertype;
   EDataType memberDataType = kNoType_t;
   const char *memberName;
   const char *memberFullTypeName;
   const char *memberTitle;
   Bool_t isapointer;
   Bool_t isbasic;

   if (TDataMember *member = cl->GetDataMember(mname)) {
      if (member->GetDataType()) {
         memberDataType = (EDataType)member->GetDataType()->GetType();
      }
      memberName = member->GetName();
      memberFullTypeName = member->GetFullTypeName();
      memberTitle = member->GetTitle();
      isapointer = member->IsaPointer();
      isbasic = member->IsBasic();
      membertype = member->GetDataType();
   } else if (!cl->IsLoaded()) {
      // The class is not loaded, hence it is 'emulated' and the main source of
      // information is the StreamerInfo.
      TVirtualStreamerInfo *info = cl->GetStreamerInfo();
      if (!info) return;
      const char *cursor = mname;
      while ( (*cursor)=='*' ) ++cursor;
      TString elname( cursor );
      Ssiz_t pos = elname.Index("[");
      if ( pos != kNPOS ) {
         elname.Remove( pos );
      }
      TStreamerElement *element = (TStreamerElement*)info->GetElements()->FindObject(elname.Data());
      if (!element) return;
      memberFullTypeName = element->GetTypeName();

      memberDataType = (EDataType)element->GetType();

      memberName = element->GetName();
      memberTitle = element->GetTitle();
      isapointer = element->IsaPointer() || element->GetType() == TVirtualStreamerInfo::kCharStar;
      membertype = gROOT->GetType(memberFullTypeName);

      isbasic = membertype !=0;
   } else {
      return;
   }


   Bool_t isdate = kFALSE;
   if (strcmp(memberName,"fDatime") == 0 && memberDataType == kUInt_t) {
      isdate = kTRUE;
   }
   Bool_t isbits = kFALSE;
   if (strcmp(memberName,"fBits") == 0 && memberDataType == kUInt_t) {
      isbits = kTRUE;
   }
   TClass * dataClass = TClass::GetClass(memberFullTypeName);
   Bool_t isTString = (dataClass == TString::Class());
   static TClassRef stdClass("std::string");
   Bool_t isStdString = (dataClass == stdClass);

   Int_t i;
   for (i = 0;i < kline; i++) line[i] = ' ';
   line[kline-1] = 0;
   snprintf(line,kline,"%s%s ",pname,mname);
   i = strlen(line); line[i] = ' ';

   // Encode data value or pointer value
   char *pointer = (char*)add;
   char **ppointer = (char**)(pointer);

   if (isapointer) {
      char **p3pointer = (char**)(*ppointer);
      if (!p3pointer)
         snprintf(&line[kvalue],kline-kvalue,"->0");
      else if (!isbasic) {
         if (!fNoAddr) {
            snprintf(&line[kvalue],kline-kvalue,"->%lx ", (Long_t)p3pointer);
         }
      } else if (membertype) {
         if (!strcmp(membertype->GetTypeName(), "char")) {
            i = strlen(*ppointer);
            if (kvalue+i > kline) i=kline-1-kvalue;
            Bool_t isPrintable = kTRUE;
            for (Int_t j = 0; j < i; j++) {
               if (!std::isprint((*ppointer)[j])) {
                  isPrintable = kFALSE;
                  break;
               }
            }
            if (isPrintable) {
               strncpy(line + kvalue, *ppointer, i);
               line[kvalue+i] = 0;
            } else {
               line[kvalue] = 0;
            }
         } else {
            strncpy(&line[kvalue], membertype->AsString(p3pointer), TMath::Min(kline-1-kvalue,(int)strlen(membertype->AsString(p3pointer))));
         }
      } else if (!strcmp(memberFullTypeName, "char*") ||
                 !strcmp(memberFullTypeName, "const char*")) {
         i = strlen(*ppointer);
         if (kvalue+i >= kline) i=kline-1-kvalue;
         Bool_t isPrintable = kTRUE;
         for (Int_t j = 0; j < i; j++) {
            if (!std::isprint((*ppointer)[j])) {
               isPrintable = kFALSE;
               break;
            }
         }
         if (isPrintable) {
            strncpy(line + kvalue, *ppointer, i);
            line[kvalue+i] = 0;
         } else {
            line[kvalue] = 0;
         }
      } else {
         if (!fNoAddr) {
            snprintf(&line[kvalue],kline-kvalue,"->%lx ", (Long_t)p3pointer);
         }
      }
   } else if (membertype) {
      if (isdate) {
         cdatime = (UInt_t*)pointer;
         TDatime::GetDateTime(cdatime[0],cdate,ctime);
         snprintf(&line[kvalue],kline-kvalue,"%d/%d",cdate,ctime);
      } else if (isbits) {
         snprintf(&line[kvalue],kline-kvalue,"0x%08x", *(UInt_t*)pointer);
      } else {
         strncpy(&line[kvalue], membertype->AsString(pointer), TMath::Min(kline-1-kvalue,(int)strlen(membertype->AsString(pointer))));
      }
   } else {
      if (isStdString) {
         std::string *str = (std::string*)pointer;
         snprintf(&line[kvalue],kline-kvalue,"%s",str->c_str());
      } else if (isTString) {
         TString *str = (TString*)pointer;
         snprintf(&line[kvalue],kline-kvalue,"%s",str->Data());
      } else {
         if (!fNoAddr) {
            snprintf(&line[kvalue],kline-kvalue,"->%lx ", (Long_t)pointer);
         }
      }
   }
   // Encode data member title
   if (isdate == kFALSE && strcmp(memberFullTypeName, "char*") && strcmp(memberFullTypeName, "const char*")) {
      i = strlen(&line[0]); line[i] = ' ';
      Int_t lentit = strlen(memberTitle);
      if (lentit > 250-ktitle) lentit = 250-ktitle;
      strncpy(&line[ktitle],memberTitle,lentit);
      line[ktitle+lentit] = 0;
   }
   Printf("%s", line);
}

THashTable* TClass::fgClassTypedefHash = 0;

//______________________________________________________________________________
//______________________________________________________________________________
//______________________________________________________________________________
TClass::TNameMapNode::TNameMapNode (const char* typedf, const char* orig)
  : TObjString (typedf),
    fOrigName (orig)
{
}

//______________________________________________________________________________

class TBuildRealData : public TMemberInspector {

private:
   void    *fRealDataObject;
   TClass  *fRealDataClass;

public:
   TBuildRealData(void *obj, TClass *cl) {
      // Main constructor.
      fRealDataObject = obj;
      fRealDataClass = cl;
   }
   using TMemberInspector::Inspect;
   void Inspect(TClass *cl, const char *parent, const char *name, const void *addr, Bool_t isTransient);

};

//______________________________________________________________________________
void TBuildRealData::Inspect(TClass* cl, const char* pname, const char* mname, const void* add, Bool_t isTransient)
{
   // This method is called from ShowMembers() via BuildRealdata().

   TDataMember* dm = cl->GetDataMember(mname);
   if (!dm) {
      return;
   }

   Bool_t isTransientMember = kFALSE;

   if (!dm->IsPersistent()) {
      // For the DataModelEvolution we need access to the transient member.
      // so we now record them in the list of RealData.
      isTransientMember = kTRUE;
      isTransient = kTRUE;
   }

   TString rname( pname );
   // Take into account cases like TPaveStats->TPaveText->TPave->TBox.
   // Check that member is in a derived class or an object in the class.
   if (cl != fRealDataClass) {
      if (!fRealDataClass->InheritsFrom(cl)) {
         Ssiz_t dot = rname.Index('.');
         if (dot == kNPOS) {
            return;
         }
         rname[dot] = '\0';
         if (!fRealDataClass->GetDataMember(rname)) {
            //could be a data member in a base class like in this example
            // class Event : public Data {
            //   class Data : public TObject {
            //     EventHeader fEvtHdr;
            //     class EventHeader {
            //       Int_t     fEvtNum;
            //       Int_t     fRun;
            //       Int_t     fDate;
            //       EventVertex fVertex;
            //       class EventVertex {
            //         EventTime  fTime;
            //         class EventTime {
            //           Int_t     fSec;
            //           Int_t     fNanoSec;
            if (!fRealDataClass->GetBaseDataMember(rname)) {
               return;
            }
         }
         rname[dot] = '.';
      }
   }
   rname += mname;
   Long_t offset = Long_t(((Long_t) add) - ((Long_t) fRealDataObject));

   if (dm->IsaPointer()) {
      // Data member is a pointer.
      if (!dm->IsBasic()) {
         // Pointer to class object.
         TRealData* rd = new TRealData(rname, offset, dm);
         if (isTransientMember) { rd->SetBit(TRealData::kTransient); };
         fRealDataClass->GetListOfRealData()->Add(rd);
      } else {
         // Pointer to basic data type.
         TRealData* rd = new TRealData(rname, offset, dm);
         if (isTransientMember) { rd->SetBit(TRealData::kTransient); };
         fRealDataClass->GetListOfRealData()->Add(rd);
      }
   } else {
      // Data Member is a basic data type.
      TRealData* rd = new TRealData(rname, offset, dm);
      if (isTransientMember) { rd->SetBit(TRealData::kTransient); };
      if (!dm->IsBasic()) {
         rd->SetIsObject(kTRUE);

         // Make sure that BuildReadData is called for any abstract
         // bases classes involved in this object, i.e for all the
         // classes composing this object (base classes, type of
         // embedded object and same for their data members).
         //
         TClass* dmclass = TClass::GetClass(dm->GetTypeName(), kTRUE, isTransient);
         if (!dmclass) {
            dmclass = TClass::GetClass(dm->GetTrueTypeName(), kTRUE, isTransient);
         }
         if (dmclass) {
            if ((dmclass != cl) && !dm->IsaPointer()) {
               if (dmclass->GetCollectionProxy()) {
                  TClass* valcl = dmclass->GetCollectionProxy()->GetValueClass();
                  // We create the real data for the content of the collection to help the case
                  // of split branches in a TTree (where the node for the data member itself
                  // might have been elided).  However, in some cases, like transient members
                  // and/or classes, the content might not be createable.   An example is the
                  // case of a map<A,B> where either A or B does not have default constructor
                  // and thus the compilation of the default constructor for pair<A,B> will
                  // fail (noisily) [This could also apply to any template instance, where it
                  // might have a default constructor definition that can not be compiled due
                  // to the template parameter]
                  if (valcl) {
                     Bool_t wantBuild = kTRUE;
                     if (valcl->Property() & kIsAbstract) wantBuild = kFALSE;
                     if ( (isTransient)
                          && (dmclass->GetCollectionProxy()->GetProperties() & TVirtualCollectionProxy::kIsEmulated)
                          && (!valcl->IsLoaded()) ) {
                        // Case where the collection dictionary was not requested and
                        // the content's dictionary was also not requested.
                        // [This is a super set of what we need, but we can't really detect it :(]
                        wantBuild = kFALSE;
                     }

                     if (wantBuild) valcl->BuildRealData(0, isTransient);
                  }
               } else {
                  void* addrForRecursion = 0;
                  if (GetObjectValidity() == kValidObjectGiven)
                     addrForRecursion = const_cast<void*>(add);

                  dmclass->BuildRealData(addrForRecursion, isTransient);
               }
            }
         }
      }
      fRealDataClass->GetListOfRealData()->Add(rd);
   }
}

//______________________________________________________________________________
//______________________________________________________________________________
//______________________________________________________________________________

//______________________________________________________________________________
class TAutoInspector : public TMemberInspector {

public:
   Int_t     fCount;
   TBrowser *fBrowser;

   TAutoInspector(TBrowser *b) {
      // main constructor.
      fBrowser = b; fCount = 0; }
   virtual ~TAutoInspector() { }
   using TMemberInspector::Inspect;
   virtual void Inspect(TClass *cl, const char *parent, const char *name, const void *addr, Bool_t isTransient);
};

//______________________________________________________________________________
void TAutoInspector::Inspect(TClass *cl, const char *tit, const char *name,
                             const void *addr, Bool_t /* isTransient */)
{
   // This method is called from ShowMembers() via AutoBrowse().

   if(tit && strchr(tit,'.'))    return ;
   if (fCount && !fBrowser) return;

   TString ts;

   if (!cl) return;
   //if (*(cl->GetName()) == 'T') return;
   if (*name == '*') name++;
   int ln = strcspn(name,"[ ");
   TString iname(name,ln);

   ClassInfo_t *classInfo = cl->GetClassInfo();
   if (!classInfo)               return;

   //              Browse data members
   DataMemberInfo_t *m = gCling->DataMemberInfo_Factory(classInfo);
   TString mname;

   int found=0;
   while (gCling->DataMemberInfo_Next(m)) {    // MemberLoop
      mname = gCling->DataMemberInfo_Name(m);
      mname.ReplaceAll("*","");
      if ((found = (iname==mname))) break;
   }
   assert(found);

   // we skip: non static members and non objects
   //  - the member G__virtualinfo inserted by the CINT RTTI system

   //Long_t prop = m.Property() | m.Type()->Property();
   Long_t prop = gCling->DataMemberInfo_Property(m) | gCling->DataMemberInfo_TypeProperty(m);
   if (prop & kIsStatic)           return;
   if (prop & kIsFundamental)      return;
   if (prop & kIsEnum)             return;
   if (mname == "G__virtualinfo")  return;

   int  size = sizeof(void*);

   int nmax = 1;
   if (prop & kIsArray) {
      for (int dim = 0; dim < gCling->DataMemberInfo_ArrayDim(m); dim++) nmax *= gCling->DataMemberInfo_MaxIndex(m,dim);
   }

   std::string clmName(TClassEdit::ShortType(gCling->DataMemberInfo_TypeName(m),
                                             TClassEdit::kDropTrailStar) );
   TClass * clm = TClass::GetClass(clmName.c_str());
   R__ASSERT(clm);
   if (!(prop & kIsPointer)) {
      size = clm->Size();
      if (size==0) size = gCling->DataMemberInfo_TypeSize(m);
   }


   gCling->DataMemberInfo_Delete(m);
   TVirtualCollectionProxy *proxy = clm->GetCollectionProxy();

   for(int i=0; i<nmax; i++) {

      char *ptr = (char*)addr + i*size;

      void *obj = (prop & kIsPointer) ? *((void**)ptr) : (TObject*)ptr;

      if (!obj)           continue;

      fCount++;
      if (!fBrowser)      return;

      TString bwname;
      TClass *actualClass = clm->GetActualClass(obj);
      if (clm->IsTObject()) {
         TObject *tobj = (TObject*)clm->DynamicCast(TObject::Class(),obj);
         bwname = tobj->GetName();
      } else {
         bwname = actualClass->GetName();
         bwname += "::";
         bwname += mname;
      }

      if (!clm->IsTObject() ||
          bwname.Length()==0 ||
          strcmp(bwname.Data(),actualClass->GetName())==0) {
         bwname = name;
         int l = strcspn(bwname.Data(),"[ ");
         if (l<bwname.Length() && bwname[l]=='[') {
            char cbuf[12]; snprintf(cbuf,12,"[%02d]",i);
            ts.Replace(0,999,bwname,l);
            ts += cbuf;
            bwname = (const char*)ts;
         }
      }

      if (proxy==0) {

         fBrowser->Add(obj,clm,bwname);

      } else {
         TClass *valueCl = proxy->GetValueClass();

         if (valueCl==0) {

            fBrowser->Add( obj, clm, bwname );

         } else {
            TVirtualCollectionProxy::TPushPop env(proxy, obj);
            TClass *actualCl = 0;

            int sz = proxy->Size();

            char fmt[] = {"#%09d"};
            fmt[3]  = '0'+(int)log10(double(sz))+1;
            char buf[20];
            for (int ii=0;ii<sz;ii++) {
               void *p = proxy->At(ii);

               if (proxy->HasPointers()) {
                  p = *((void**)p);
                  if(!p) continue;
                  actualCl = valueCl->GetActualClass(p);
                  p = actualCl->DynamicCast(valueCl,p,0);
               }
               fCount++;
               snprintf(buf,20,fmt,ii);
               ts = bwname;
               ts += buf;
               fBrowser->Add( p, actualCl, ts );
            }
         }
      }
   }
}

//______________________________________________________________________________
//______________________________________________________________________________
//______________________________________________________________________________

ClassImp(TClass)

//______________________________________________________________________________
TClass::TClass() :
   TDictionary(),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kNoInfo),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)

{
   // Default ctor.

   R__LOCKGUARD2(gInterpreterMutex);
   fDeclFileLine   = -2;    // -2 for standalone TClass (checked in dtor)
}

//______________________________________________________________________________
TClass::TClass(const char *name, Bool_t silent) :
   TDictionary(name),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kNoInfo),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.
   // Use this ctor to create a standalone TClass object. Most useful
   // to get a TClass interface to an interpreted class. Used by TTabCom.
   // Normally you would use TClass::GetClass("class") to get access to a
   // TClass object for a certain class.

   R__LOCKGUARD2(gInterpreterMutex);

   if (!gROOT)
      ::Fatal("TClass::TClass", "ROOT system not initialized");

   fDeclFileLine   = -2;    // -2 for standalone TClass (checked in dtor)

   SetBit(kLoading);
   if (!gInterpreter)
      ::Fatal("TClass::TClass", "gInterpreter not initialized");

   gInterpreter->SetClassInfo(this);   // sets fClassInfo pointer
   if (!silent && !fClassInfo && fName.First('@')==kNPOS)
      ::Warning("TClass::TClass", "no dictionary for class %s is available", name);
   ResetBit(kLoading);

   if (fClassInfo) SetTitle(gCling->ClassInfo_Title(fClassInfo));
   fConversionStreamerInfo = 0;
}

//______________________________________________________________________________
TClass::TClass(const char *name, Version_t cversion, Bool_t silent) :
   TDictionary(name),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kNoInfo),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.
   R__LOCKGUARD2(gInterpreterMutex);
   Init(name, cversion, 0, 0, 0, 0, -1, -1, 0, silent);
}

//______________________________________________________________________________
TClass::TClass(const char *name, Version_t cversion, EState theState, Bool_t silent) :
   TDictionary(name),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(theState),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object does not contain anything. We mimic
   // the case of a class fwd declared in the interpreter.
   R__LOCKGUARD2(gInterpreterMutex);

   // Treat the case in which a TClass instance is created for a namespace
   if (theState == kNamespaceForMeta){
      fProperty = kIsNamespace;
      theState = kForwardDeclared; // it immediately decays in kForwardDeclared
   }

   if (theState != kForwardDeclared && theState != kEmulated)
      ::Fatal("TClass::TClass",
              "A TClass entry cannot be initialized in a state different from kForwardDeclared or kEmulated.");
   Init(name, cversion, 0, 0, 0, 0, -1, -1, 0, silent);
}

//______________________________________________________________________________
TClass::TClass(ClassInfo_t *classInfo, Version_t cversion,
               const char *dfil, const char *ifil, Int_t dl, Int_t il, Bool_t silent) :
   TDictionary(""),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kNoInfo),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.
   // Use this ctor to create a standalone TClass object. Most useful
   // to get a TClass interface to an interpreted class. Used by TTabCom.
   // Normally you would use TClass::GetClass("class") to get access to a
   // TClass object for a certain class.
   //
   // This copies the ClassInfo (i.e. does *not* take ownership of it).

   R__LOCKGUARD2(gInterpreterMutex);

   if (!gROOT)
      ::Fatal("TClass::TClass", "ROOT system not initialized");

   fDeclFileLine   = -2;    // -2 for standalone TClass (checked in dtor)

   SetBit(kLoading);
   if (!gInterpreter)
      ::Fatal("TClass::TClass", "gInterpreter not initialized");

   if (!classInfo || !gInterpreter->ClassInfo_IsValid(classInfo)) {
      MakeZombie();
      fState = kNoInfo;
   } else {
      fName = gInterpreter->ClassInfo_FullName(classInfo);

      R__LOCKGUARD2(gInterpreterMutex);
      Init(fName, cversion, 0, 0, dfil, ifil, dl, il, classInfo, silent);
   }
   ResetBit(kLoading);

   fConversionStreamerInfo = 0;
}


//______________________________________________________________________________
TClass::TClass(const char *name, Version_t cversion,
               const char *dfil, const char *ifil, Int_t dl, Int_t il, Bool_t silent) :
   TDictionary(name),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0), fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kNoInfo),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.
   R__LOCKGUARD2(gInterpreterMutex);
   Init(name,cversion, 0, 0, dfil, ifil, dl, il, 0, silent);
}

//______________________________________________________________________________
TClass::TClass(const char *name, Version_t cversion,
               const type_info &info, TVirtualIsAProxy *isa,
               const char *dfil, const char *ifil, Int_t dl, Int_t il,
               Bool_t silent) :
   TDictionary(name),
   fPersistentRef(0),
   fStreamerInfo(0), fConversionStreamerInfo(0), fRealData(0),
   fBase(0), fData(0), fEnums(0), fFuncTemplate(0), fMethod(0), fAllPubData(0),
   fAllPubMethod(0),
   fClassMenuList(0),
   fDeclFileName(""), fImplFileName(""), fDeclFileLine(0), fImplFileLine(0),
   fInstanceCount(0), fOnHeap(0),
   fCheckSum(0), fCollectionProxy(0), fClassVersion(0), fClassInfo(0),
   fTypeInfo(0), fShowMembers(0),
   fStreamer(0), fIsA(0), fGlobalIsA(0), fIsAMethod(0),
   fMerge(0), fResetAfterMerge(0), fNew(0), fNewArray(0), fDelete(0), fDeleteArray(0),
   fDestructor(0), fDirAutoAdd(0), fStreamerFunc(0), fSizeof(-1),
   fCanSplit(-1), fProperty(0), fClassProperty(0), fHasRootPcmInfo(kFALSE), fCanLoadClassInfo(kFALSE),
   fIsOffsetStreamerSet(kFALSE), fVersionUsed(kFALSE), fOffsetStreamer(0), fStreamerType(TClass::kDefault),
   fState(kHasTClassInit),
   fCurrentInfo(0), fLastReadInfo(0), fRefProxy(0),
   fSchemaRules(0), fStreamerImpl(&TClass::StreamerDefault)
{
   // Create a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.

   R__LOCKGUARD2(gInterpreterMutex);
   // use info
   Init(name, cversion, &info, isa, dfil, ifil, dl, il, 0, silent);
}

//______________________________________________________________________________
void TClass::ForceReload (TClass* oldcl)
{
   // we found at least one equivalent.
   // let's force a reload

   TClass::RemoveClass(oldcl);

   if (oldcl->CanIgnoreTObjectStreamer()) {
      IgnoreTObjectStreamer();
   }

   TVirtualStreamerInfo *info;
   TIter next(oldcl->GetStreamerInfos());
   while ((info = (TVirtualStreamerInfo*)next())) {
      info->Clear("build");
      info->SetClass(this);
      fStreamerInfo->AddAtAndExpand(info,info->GetClassVersion());
   }
   oldcl->fStreamerInfo->Clear();

   oldcl->ReplaceWith(this);
   delete oldcl;
}

//______________________________________________________________________________
void TClass::Init(const char *name, Version_t cversion,
                  const type_info *typeinfo, TVirtualIsAProxy *isa,
                  const char *dfil, const char *ifil, Int_t dl, Int_t il,
                  ClassInfo_t *givenInfo,
                  Bool_t silent)
{
   // Initialize a TClass object. This object contains the full dictionary
   // of a class. It has list to baseclasses, datamembers and methods.
   if (!gROOT)
      ::Fatal("TClass::TClass", "ROOT system not initialized");

   // Always strip the default STL template arguments (from any template argument or the class name)
   fName           = TClassEdit::ShortType(name, TClassEdit::kDropStlDefault).c_str();
   fClassVersion   = cversion;
   fDeclFileName   = dfil ? dfil : "";
   fImplFileName   = ifil ? ifil : "";
   fDeclFileLine   = dl;
   fImplFileLine   = il;
   fTypeInfo       = typeinfo;
   fIsA            = isa;
   if ( fIsA ) fIsA->SetClass(this);
   // See also TCling::GenerateTClass() which will update fClassVersion after creation!
   fStreamerInfo   = new TObjArray(fClassVersion+2+10,-1); // +10 to read new data by old
   fProperty       = -1;
   fClassProperty  = -1;

   ResetInstanceCount();

   TClass *oldcl = (TClass*)gROOT->GetListOfClasses()->FindObject(fName.Data());

   InsertTClassInRegistryRAII insertRAII(fState,fName,fNoInfoOrEmuOrFwdDeclNameRegistry);

   if (oldcl && oldcl->TestBit(kLoading)) {
      // Do not recreate a class while it is already being created!

      // We can no longer reproduce this case, to check whether we are, we use
      // this code:
      // Fatal("Init","A bad replacement for %s was requested\n",name);
      return;
   }

   TClass **persistentRef = 0;
   if (oldcl) {

      persistentRef = oldcl->fPersistentRef.exchange(0);

      // The code from here is also in ForceReload.
      TClass::RemoveClass(oldcl);
      // move the StreamerInfo immediately so that there are
      // properly updated!

      if (oldcl->CanIgnoreTObjectStreamer()) {
         IgnoreTObjectStreamer();
      }
      TVirtualStreamerInfo *info;

      TIter next(oldcl->GetStreamerInfos());
      while ((info = (TVirtualStreamerInfo*)next())) {
         // We need to force a call to BuildOld
         info->Clear("build");
         info->SetClass(this);
         fStreamerInfo->AddAtAndExpand(info,info->GetClassVersion());
      }
      oldcl->fStreamerInfo->Clear();
      // The code diverges here from ForceReload.

      // Move the Schema Rules too.
      fSchemaRules = oldcl->fSchemaRules;
      oldcl->fSchemaRules = 0;
   }

   SetBit(kLoading);
   // Advertise ourself as the loading class for this class name
   TClass::AddClass(this);

   Bool_t isStl = TClassEdit::IsSTLCont(fName);

   if (!gInterpreter) {
      ::Fatal("TClass::Init", "gInterpreter not initialized");
   }

   if (givenInfo) {
      if (!gInterpreter->ClassInfo_IsValid(givenInfo) ||
          !(gInterpreter->ClassInfo_Property(givenInfo) & (kIsClass | kIsStruct | kIsNamespace)) ||
          (!gInterpreter->ClassInfo_IsLoaded(givenInfo) && (gInterpreter->ClassInfo_Property(givenInfo) & (kIsNamespace))) )
      {
         if (!TClassEdit::IsSTLCont(fName.Data())) {
            MakeZombie();
            fState = kNoInfo;
            TClass::RemoveClass(this);
            return;
         }
      }
      fClassInfo = gInterpreter->ClassInfo_Factory(givenInfo);
   }
   // We need to check if the class it is not fwd declared for the cases where we
   // created a TClass directly in the kForwardDeclared state. Indeed in those cases
   // fClassInfo will always be nullptr.
   if (fState!=kForwardDeclared && !fClassInfo) {

      if (fState == kHasTClassInit) {
         // If the TClass is being generated from a ROOT dictionary,
         // eventhough we do not seem to have a CINT dictionary for
         // the class, we will will try to load it anyway UNLESS
         // the class is an STL container (or string).
         // This is because we do not expect the CINT dictionary
         // to be present for all STL classes (and we can handle
         // the lack of CINT dictionary in that cases).
         // However, we cling the dictionary no longer carries
         // an instantiation with it, unless we request the loading
         // here *or* the user explicitly instantiate the template
         // we would not have a ClassInfo for the template
         // instantiation.
         fCanLoadClassInfo = kTRUE;
         // Here we check and grab the info from the rootpcm.
         TProtoClass *proto = TClassTable::GetProtoNorm(GetName());
         if (proto && proto->FillTClass(this)) {
            fHasRootPcmInfo = kTRUE;
         }
      }
      if (!fHasRootPcmInfo && gInterpreter->CheckClassInfo(fName, /* autoload = */ kTRUE)) {
         gInterpreter->SetClassInfo(this);   // sets fClassInfo pointer
         if (!fClassInfo) {
            if (IsZombie()) {
               TClass::RemoveClass(this);
               return;
            }
         }
      }
   }
   if (!silent && (!fClassInfo && !fCanLoadClassInfo) && !isStl && fName.First('@')==kNPOS &&
       !TClassEdit::IsInterpreterDetail(fName.Data()) ) {
      if (fState == kHasTClassInit) {
         ::Error("TClass::Init", "no interpreter information for class %s is available eventhough it has a TClass initialization routine.", fName.Data());
      } else {
         // In this case we initialised this TClass instance starting from the fwd declared state
         // and we know we have no dictionary: no need to warn
         ::Warning("TClass::Init", "no dictionary for class %s is available", fName.Data());
      }
   }

   fgClassCount++;
   SetUniqueID(fgClassCount);

   // Make the typedef-expanded -> original hash table entries.
   // There may be several entries for any given key.
   // We only make entries if the typedef-expanded name
   // is different from the original name.
   TString resolvedThis;
   if (!givenInfo && strchr (name, '<')) {
      if ( fName != name) {
         if (!fgClassTypedefHash) {
            fgClassTypedefHash = new THashTable (100, 5);
            fgClassTypedefHash->SetOwner (kTRUE);
         }

         fgClassTypedefHash->Add (new TNameMapNode (name, fName));
         SetBit (kHasNameMapNode);

      }
      resolvedThis = TClassEdit::ResolveTypedef (name, kTRUE);
      if (resolvedThis != name) {
         if (!fgClassTypedefHash) {
            fgClassTypedefHash = new THashTable (100, 5);
            fgClassTypedefHash->SetOwner (kTRUE);
         }

         fgClassTypedefHash->Add (new TNameMapNode (resolvedThis, fName));
         SetBit (kHasNameMapNode);
      }

   }

   //In case a class with the same name had been created by TVirtualStreamerInfo
   //we must delete the old class, importing only the StreamerInfo structure
   //from the old dummy class.
   if (oldcl) {

      oldcl->ReplaceWith(this);
      delete oldcl;

   } else if (!givenInfo && resolvedThis.Length() > 0 && fgClassTypedefHash) {

      // Check for existing equivalent.

      if (resolvedThis != fName) {
         oldcl = (TClass*)gROOT->GetListOfClasses()->FindObject(resolvedThis);
         if (oldcl && oldcl != this) {
            persistentRef = oldcl->fPersistentRef.exchange(0);
            ForceReload (oldcl);
         }
      }
      TIter next( fgClassTypedefHash->GetListForObject(resolvedThis) );
      while ( TNameMapNode* htmp = static_cast<TNameMapNode*> (next()) ) {
         if (resolvedThis != htmp->String()) continue;
         oldcl = (TClass*)gROOT->GetListOfClasses()->FindObject(htmp->fOrigName); // gROOT->GetClass (htmp->fOrigName, kFALSE);
         if (oldcl && oldcl != this) {
            persistentRef = oldcl->fPersistentRef.exchange(0);
            ForceReload (oldcl);
         }
      }
   }
   if (fClassInfo) {
      SetTitle(gCling->ClassInfo_Title(fClassInfo));
      if ( fDeclFileName == 0 || fDeclFileName[0] == '\0' ) {
         fDeclFileName = gInterpreter->ClassInfo_FileName( fClassInfo );
         // Missing interface:
         // fDeclFileLine = gInterpreter->ClassInfo_FileLine( fClassInfo );

         // But really do not want to set ImplFileLine as it is currently the
         // marker of being 'loaded' or not (reminder loaded == has a TClass bootstrap).
      }
   }

   if (persistentRef) {
      fPersistentRef = persistentRef;
   } else {
      fPersistentRef = new TClass*;
   }
   *fPersistentRef = this;

   if ( isStl || !strncmp(GetName(),"stdext::hash_",13) || !strncmp(GetName(),"__gnu_cxx::hash_",16) ) {
      if (fState != kHasTClassInit) {
         // If we have a TClass compiled initialization, we can safely assume that
         // there will also be a collection proxy.
         fCollectionProxy = TVirtualStreamerInfo::Factory()->GenEmulatedProxy( GetName(), silent );
         if (fCollectionProxy) {
            fSizeof = fCollectionProxy->Sizeof();

            // Numeric Collections have implicit conversions:
            GetSchemaRules(kTRUE);

         } else if (!silent) {
            Warning("Init","Collection proxy for %s was not properly initialized!",GetName());
         }
         if (fStreamer==0) {
            fStreamer =  TVirtualStreamerInfo::Factory()->GenEmulatedClassStreamer( GetName(), silent );
         }
      }
   } else if (!strncmp(GetName(),"std::pair<",10) || !strncmp(GetName(),"pair<",5) ) {
      // std::pairs have implicit conversions
      GetSchemaRules(kTRUE);
   }

   ResetBit(kLoading);
}

//______________________________________________________________________________
TClass::~TClass()
{
   // TClass dtor. Deletes all list that might have been created.

   R__LOCKGUARD(gInterpreterMutex);

   // Remove from the typedef hashtables.
   if (fgClassTypedefHash && TestBit (kHasNameMapNode)) {
      TString resolvedThis = TClassEdit::ResolveTypedef (GetName(), kTRUE);
      TIter next (fgClassTypedefHash->GetListForObject (resolvedThis));
      while ( TNameMapNode* htmp = static_cast<TNameMapNode*> (next()) ) {
         if (resolvedThis == htmp->String() && htmp->fOrigName == GetName()) {
            fgClassTypedefHash->Remove (htmp);
            delete htmp;
            break;
         }
      }
   }

   // Not owning lists, don't call Delete()
   // But this still need to be done first because the TList desctructor
   // does access the object contained (via GetObject()->TestBit(kCanDelete))
   delete fStreamer;       fStreamer    =0;
   delete fAllPubData;     fAllPubData  =0;
   delete fAllPubMethod;   fAllPubMethod=0;

   delete fPersistentRef.load();

   if (fBase)
      fBase->Delete();
   delete fBase;   fBase=0;

   if (fData)
      fData->Delete();
   delete fData;   fData = 0;

   if (fEnums)
      fEnums->Delete();
   delete fEnums; fEnums = 0;

   if (fFuncTemplate)
      fFuncTemplate->Delete();
   delete fFuncTemplate; fFuncTemplate = 0;

   if (fMethod)
      fMethod->Delete();
   delete fMethod;   fMethod=0;

   if (fRealData)
      fRealData->Delete();
   delete fRealData;  fRealData=0;

   if (fStreamerInfo)
      fStreamerInfo->Delete();
   delete fStreamerInfo; fStreamerInfo=0;

   if (fDeclFileLine >= -1)
      TClass::RemoveClass(this);

   gCling->ClassInfo_Delete(fClassInfo);
   fClassInfo=0;

   if (fClassMenuList)
      fClassMenuList->Delete();
   delete fClassMenuList; fClassMenuList=0;

   fIsOffsetStreamerSet=kFALSE;

   if ( fIsA ) delete fIsA;

   if ( fRefProxy ) fRefProxy->Release();
   fRefProxy = 0;

   delete fStreamer;
   delete fCollectionProxy;
   delete fIsAMethod.load();
   delete fSchemaRules;
   if (fConversionStreamerInfo.load()) {
      std::map<std::string, TObjArray*>::iterator it;
      std::map<std::string, TObjArray*>::iterator end = (*fConversionStreamerInfo).end();
      for( it = (*fConversionStreamerInfo).begin(); it != end; ++it ) {
         delete it->second;
      }
      delete fConversionStreamerInfo.load();
   }
}

//------------------------------------------------------------------------------
namespace {
   Int_t ReadRulesContent(FILE *f)
   {
      // Read a class.rules file which contains one rule per line with comment
      // starting with a #
      // Returns the number of rules loaded.
      // Returns -1 in case of error.

      R__ASSERT(f!=0);
      TString rule(1024);
      int c, state = 0;
      Int_t count = 0;

      while ((c = fgetc(f)) != EOF) {
         if (c == 13)        // ignore CR
            continue;
         if (c == '\n') {
            if (state != 3) {
               state = 0;
               if (rule.Length() > 0) {
                  if (TClass::AddRule(rule)) {
                     ++count;
                  }
                  rule.Clear();
               }
            }
            continue;
         }
         switch (state) {
            case 0:             // start of line
               switch (c) {
                  case ' ':
                  case '\t':
                     break;
                  case '#':
                     state = 1;
                     break;
                  default:
                     state = 2;
                     break;
               }
               break;

            case 1:             // comment
               break;

            case 2:             // rule
               switch (c) {
                  case '\\':
                     state = 3; // Continuation request
                  default:
                     break;
               }
               break;
         }
         switch (state) {
            case 2:
               rule.Append(c);
               break;
         }
      }
      return count;
   }
}

//------------------------------------------------------------------------------
Int_t TClass::ReadRules()
{
   // Read the class.rules files from the default location:.
   //     $ROOTSYS/etc/class.rules (or ROOTETCDIR/class.rules)

   static const char *suffix = "class.rules";
   TString sname = suffix;
#ifdef ROOTETCDIR
   gSystem->PrependPathName(ROOTETCDIR, sname);
#else
   TString etc = gRootDir;
#ifdef WIN32
   etc += "\\etc";
#else
   etc += "/etc";
#endif
   gSystem->PrependPathName(etc, sname);
#endif

   Int_t res = -1;

   FILE * f = fopen(sname,"r");
   if (f != 0) {
      res = ReadRulesContent(f);
      fclose(f);
   }
   return res;
}

//------------------------------------------------------------------------------
Int_t TClass::ReadRules( const char *filename )
{
   // Read a class.rules file which contains one rule per line with comment
   // starting with a #
   // Returns the number of rules loaded.
   // Returns -1 in case of error.

   if (!filename || !filename[0]) {
      ::Error("TClass::ReadRules", "no file name specified");
      return -1;
   }

   FILE * f = fopen(filename,"r");
   if (f == 0) {
      ::Error("TClass::ReadRules","Failed to open %s\n",filename);
      return -1;
   }
   Int_t count = ReadRulesContent(f);

   fclose(f);
   return count;

}

//------------------------------------------------------------------------------
Bool_t TClass::AddRule( const char *rule )
{
   // Add a schema evolution customization rule.
   // The syntax of the rule can be either the short form:
   //  [type=Read] classname membername [attributes=... ] [version=[...] ] [checksum=[...] ] [oldtype=...] [code={...}]
   // or the long form
   //  [type=Read] sourceClass=classname [targetclass=newClassname] [ source="type membername; [type2 membername2]" ]
   //      [target="membername3;membername4"] [attributes=... ] [version=...] [checksum=...] [code={...}|functionname]
   //
   // For example to set HepMC::GenVertex::m_event to _not_ owned the object it is pointing to:
   //   HepMC::GenVertex m_event attributes=NotOwner
   //
   // Semantic of the tags:
   //   type : the type of the rule, valid values: Read, ReadRaw, Write, WriteRaw, the default is 'Read'.
   //   sourceClass : the name of the class as it is on the rule file
   //   targetClass : the name of the class as it is in the current code ; defaults to the value of sourceClass
   //   source : the types and names of the data members from the class on file that are needed, the list is separated by semi-colons ';'
   //   oldtype: in the short form only, indicates the type on disk of the data member.
   //   target : the names of the data members updated by this rule, the list is separated by semi-colons ';'
   //   attributes : list of possible qualifiers amongs:
   //      Owner, NotOwner
   //   version : list of the version of the class layout that this rule applies to.  The syntax can be [1,4,5] or [2-] or [1-3] or [-3]
   //   checksum : comma delimited list of the checksums of the class layout that this rule applies to.
   //   code={...} : code to be executed for the rule or name of the function implementing it.

   ROOT::TSchemaRule *ruleobj = new ROOT::TSchemaRule();
   if (! ruleobj->SetFromRule( rule ) ) {
      delete ruleobj;
      return kFALSE;
   }

   R__LOCKGUARD(gInterpreterMutex);

   TClass *cl = TClass::GetClass( ruleobj->GetTargetClass() );
   if (!cl) {
      // Create an empty emulated class for now.
      cl = gInterpreter->GenerateTClass(ruleobj->GetTargetClass(), /* emulation = */ kTRUE, /*silent = */ kTRUE);
   }
   ROOT::TSchemaRuleSet* rset = cl->GetSchemaRules( kTRUE );

   TString errmsg;
   if( !rset->AddRule( ruleobj, ROOT::TSchemaRuleSet::kCheckConflict, &errmsg ) ) {
      ::Warning( "TClass::AddRule", "The rule for class: \"%s\": version, \"%s\" and data members: \"%s\" has been skipped because it conflicts with one of the other rules (%s).",
                ruleobj->GetTargetClass(), ruleobj->GetVersion(), ruleobj->GetTargetString(), errmsg.Data() );
      delete ruleobj;
      return kFALSE;
   }
   return kTRUE;
}

//------------------------------------------------------------------------------
void TClass::AdoptSchemaRules( ROOT::TSchemaRuleSet *rules )
{
   // Adopt a new set of Data Model Evolution rules.

   R__LOCKGUARD(gInterpreterMutex);

   delete fSchemaRules;
   fSchemaRules = rules;
   fSchemaRules->SetClass( this );
}

//------------------------------------------------------------------------------
const ROOT::TSchemaRuleSet* TClass::GetSchemaRules() const
{
   // Return the set of the schema rules if any.
   return fSchemaRules;
}

//------------------------------------------------------------------------------
ROOT::TSchemaRuleSet* TClass::GetSchemaRules(Bool_t create)
{
   // Return the set of the schema rules if any.
   // If create is true, create an empty set
   if (create && fSchemaRules == 0) {
      fSchemaRules = new ROOT::TSchemaRuleSet();
      fSchemaRules->SetClass( this );
   }
   return fSchemaRules;
}

//______________________________________________________________________________
void TClass::AddImplFile(const char* filename, int line) {

   // Currently reset the implementation file and line.
   // In the close future, it will actually add this file and line
   // to a "list" of implementation files.

   fImplFileName = filename;
   fImplFileLine = line;
}

//______________________________________________________________________________
Int_t TClass::AutoBrowse(TObject *obj, TBrowser *b)
{
   // Browse external object inherited from TObject.
   // It passes through inheritance tree and calls TBrowser::Add
   // in appropriate cases. Static function.

   if (!obj) return 0;

   TAutoInspector insp(b);
   obj->ShowMembers(insp);
   return insp.fCount;
}

//______________________________________________________________________________
Int_t TClass::Browse(void *obj, TBrowser *b) const
{
   // Browse objects of of the class described by this TClass object.

   if (!obj) return 0;

   TClass *actual = GetActualClass(obj);
   if (IsTObject()) {
      // Call TObject::Browse.

      if (!fIsOffsetStreamerSet) {
         CalculateStreamerOffset();
      }
      TObject* realTObject = (TObject*)((size_t)obj + fOffsetStreamer);
      realTObject->Browse(b);
      return 1;
   } else if (actual != this) {
      return actual->Browse(obj, b);
   } else if (GetCollectionProxy()) {

      // do something useful.

   } else {
      TAutoInspector insp(b);
      CallShowMembers(obj,insp,kFALSE);
      return insp.fCount;
   }

   return 0;
}

//______________________________________________________________________________
void TClass::Browse(TBrowser *b)
{
   // This method is called by a browser to get the class information.

   if (!HasInterpreterInfo()) return;

   if (b) {
      if (!fRealData) BuildRealData();

      b->Add(GetListOfDataMembers(), "Data Members");
      b->Add(GetListOfRealData(), "Real Data Members");
      b->Add(GetListOfMethods(), "Methods");
      b->Add(GetListOfBases(), "Base Classes");
   }
}

//______________________________________________________________________________
void TClass::BuildRealData(void* pointer, Bool_t isTransient)
{
   // Build a full list of persistent data members.
   // Scans the list of all data members in the class itself and also
   // in all base classes. For each persistent data member, inserts a
   // TRealData object in the list fRealData.
   //


   R__LOCKGUARD(gInterpreterMutex);

   // Only do this once.
   if (fRealData) {
      return;
   }

   if (fClassVersion == 0) {
      isTransient = kTRUE;
   }

   // When called via TMapFile (e.g. Update()) make sure that the dictionary
   // gets allocated on the heap and not in the mapped file.
   TMmallocDescTemp setreset;

   // Handle emulated classes and STL containers specially.
   if (!HasInterpreterInfo() || TClassEdit::IsSTLCont(GetName(), 0) || TClassEdit::IsSTLBitset(GetName())) {
      // We are an emulated class or an STL container.
      fRealData = new TList;
      BuildEmulatedRealData("", 0, this);
      return;
   }

   // return early on string
   static TClassRef clRefString("std::string");
   if (clRefString == this) {
      return;
   }

   // Complain about stl classes ending up here (unique_ptr etc) - except for
   // pair where we will build .first, .second just fine
   // and those for which the user explicitly requested a dictionary.
   if (!isTransient && GetState() != kHasTClassInit
       && TClassEdit::IsStdClass(GetName())
       && strncmp(GetName(), "pair<", 5) != 0) {
      Error("BuildRealData", "Inspection for %s not supported!", GetName());
   }

   // The following statement will recursively call
   // all the subclasses of this class.
   fRealData = new TList;
   TBuildRealData brd(pointer, this);

   // CallShowMember will force a call to InheritsFrom, which indirectly
   // calls TClass::GetClass.  It forces the loading of new typedefs in
   // case some of them were not yet loaded.
   if ( ! CallShowMembers(pointer, brd, isTransient) ) {
      if ( isTransient ) {
         // This is a transient data member, so it is probably fine to not have
         // access to its content.  However let's no mark it as definitively setup,
         // since another class might use this class for a persistent data member and
         // in this case we really want the error message.
         delete fRealData;
         fRealData = 0;
      } else {
         Error("BuildRealData", "Cannot find any ShowMembers function for %s!", GetName());
      }
   }

   // Take this opportunity to build the real data for base classes.
   // In case one base class is abstract, it would not be possible later
   // to create the list of real data for this abstract class.
   TBaseClass* base = 0;
   TIter next(GetListOfBases());
   while ((base = (TBaseClass*) next())) {
      if (base->IsSTLContainer()) {
         continue;
      }
      TClass* c = base->GetClassPointer();
      if (c) {
         c->BuildRealData(0, isTransient);
      }
   }
}

//______________________________________________________________________________
void TClass::BuildEmulatedRealData(const char *name, Long_t offset, TClass *cl)
{
   // Build the list of real data for an emulated class

   R__LOCKGUARD(gInterpreterMutex);

   TVirtualStreamerInfo *info;
   if (Property() & kIsAbstract) {
      info = GetStreamerInfoAbstractEmulated();
   } else {
      info = GetStreamerInfo();
   }
   if (!info) {
      // This class is abstract, but we don't yet have a SteamerInfo for it ...
      Error("BuildEmulatedRealData","Missing StreamerInfo for %s",GetName());
      // Humm .. no information ... let's bail out
      return;
   }

   TIter next(info->GetElements());
   TStreamerElement *element;
   while ((element = (TStreamerElement*)next())) {
      Int_t etype    = element->GetType();
      Long_t eoffset = element->GetOffset();
      TClass *cle    = element->GetClassPointer();
      if (element->IsBase() || etype == TVirtualStreamerInfo::kBase) {
         //base class are skipped in this loop, they will be added at the end.
         continue;
      } else if (etype == TVirtualStreamerInfo::kTObject ||
                 etype == TVirtualStreamerInfo::kTNamed ||
                 etype == TVirtualStreamerInfo::kObject ||
                 etype == TVirtualStreamerInfo::kAny) {
         //member class
         TString rdname; rdname.Form("%s%s",name,element->GetFullName());
         TRealData *rd = new TRealData(rdname,offset+eoffset,0);
         if (gDebug > 0) printf(" Class: %s, adding TRealData=%s, offset=%ld\n",cl->GetName(),rd->GetName(),rd->GetThisOffset());
         cl->GetListOfRealData()->Add(rd);
         // Now we a dot
         rdname.Form("%s%s.",name,element->GetFullName());
         if (cle) cle->BuildEmulatedRealData(rdname,offset+eoffset,cl);
      } else {
         //others
         TString rdname; rdname.Form("%s%s",name,element->GetFullName());
         TRealData *rd = new TRealData(rdname,offset+eoffset,0);
         if (gDebug > 0) printf(" Class: %s, adding TRealData=%s, offset=%ld\n",cl->GetName(),rd->GetName(),rd->GetThisOffset());
         cl->GetListOfRealData()->Add(rd);
      }
      //if (fClassInfo==0 && element->IsBase()) {
      //   if (fBase==0) fBase = new TList;
      //   TClass *base = element->GetClassPointer();
      //   fBase->Add(new TBaseClass(this, cl, eoffset));
      //}
   }
   // The base classes must added last on the list of real data (to help with ambiguous data member names)
   next.Reset();
   while ((element = (TStreamerElement*)next())) {
      Int_t etype    = element->GetType();
      if (element->IsBase() || etype == TVirtualStreamerInfo::kBase) {
         //base class
         Long_t eoffset = element->GetOffset();
         TClass *cle    = element->GetClassPointer();
         if (cle) cle->BuildEmulatedRealData(name,offset+eoffset,cl);
      }
   }
}


//______________________________________________________________________________
void TClass::CalculateStreamerOffset() const
{
   // Calculate the offset between an object of this class to
   // its base class TObject. The pointer can be adjusted by
   // that offset to access any virtual method of TObject like
   // Streamer() and ShowMembers().
   R__LOCKGUARD(gInterpreterMutex);
   if (!fIsOffsetStreamerSet && HasInterpreterInfo()) {
      // When called via TMapFile (e.g. Update()) make sure that the dictionary
      // gets allocated on the heap and not in the mapped file.

      TMmallocDescTemp setreset;
      fOffsetStreamer = const_cast<TClass*>(this)->GetBaseClassOffsetRecurse(TObject::Class());
      if (fStreamerType == kTObject) {
         fStreamerImpl = &TClass::StreamerTObjectInitialized;
      }
      fIsOffsetStreamerSet = kTRUE;
   }
}


//______________________________________________________________________________
Bool_t TClass::CallShowMembers(const void* obj, TMemberInspector &insp, Bool_t isTransient) const
{
   // Call ShowMembers() on the obj of this class type, passing insp and parent.
   // isATObject is -1 if unknown, 0 if it is not a TObject, and 1 if it is a TObject.
   // The function returns whether it was able to call ShowMembers().

   if (fShowMembers) {
      // This should always works since 'pointer' should be pointing
      // to an object of the actual type of this TClass object.
      fShowMembers(obj, insp, isTransient);
      return kTRUE;
   } else {

      if (fCanLoadClassInfo) LoadClassInfo();
      if (fClassInfo) {

         if (strcmp(GetName(), "string") == 0) {
            // For std::string we know that we do not have a ShowMembers
            // function and that it's okay.
            return kTRUE;
         }
         // Since we do have some dictionary information, let's
         // call the interpreter's ShowMember.
         // This works with Cling to support interpreted classes.
         gInterpreter->InspectMembers(insp, obj, this, isTransient);
         return kTRUE;

      } else if (TVirtualStreamerInfo* sinfo = GetStreamerInfo()) {
         sinfo->CallShowMembers(obj, insp, isTransient);
         return kTRUE;
      } // isATObject
   } // fShowMembers is set

   return kFALSE;
}

//______________________________________________________________________________
void TClass::InterpretedShowMembers(void* obj, TMemberInspector &insp, Bool_t isTransient)
{
   // Do a ShowMembers() traversal of all members and base classes' members
   // using the reflection information from the interpreter. Works also for
   // interpreted objects.

   return gInterpreter->InspectMembers(insp, obj, this, isTransient);
}

//______________________________________________________________________________
Bool_t TClass::CanSplit() const
{
   // Return true if the data member of this TClass can be saved separately.

   // Note: add the possibility to set it for the class and the derived class.
   // save the info in TVirtualStreamerInfo
   // deal with the info in MakeProject
   if (fCanSplit >= 0) {
      // The user explicitly set the value
      return fCanSplit != 0;
   }

   R__LOCKGUARD(gInterpreterMutex);
   TClass *This = const_cast<TClass*>(this);

   if (this == TObject::Class())  { This->fCanSplit = 1; return kTRUE; }
   if (fName == "TClonesArray")   { This->fCanSplit = 1; return kTRUE; }
   if (fRefProxy)                 { This->fCanSplit = 0; return kFALSE; }
   if (fName.BeginsWith("TVectorT<")) { This->fCanSplit = 0; return kFALSE; }
   if (fName.BeginsWith("TMatrixT<")) { This->fCanSplit = 0; return kFALSE; }
   if (fName == "string")         { This->fCanSplit = 0; return kFALSE; }
   if (fName == "std::string")    { This->fCanSplit = 0; return kFALSE; }

   if (GetCollectionProxy()!=0) {
      // For STL collection we need to look inside.

      // However we do not split collections of collections
      // nor collections of strings
      // nor collections of pointers (unless explicit request (see TBranchSTL)).

      if (GetCollectionProxy()->HasPointers()) { This->fCanSplit = 0; return kFALSE; }

      TClass *valueClass = GetCollectionProxy()->GetValueClass();
      if (valueClass == 0) { This->fCanSplit = 0; return kFALSE; }
      static TClassRef stdStringClass("std::string");
      if (valueClass==TString::Class() || valueClass==stdStringClass)
         { This->fCanSplit = 0; return kFALSE; }
      if (!valueClass->CanSplit()) { This->fCanSplit = 0; return kFALSE; }
      if (valueClass->GetCollectionProxy() != 0) { This->fCanSplit = 0; return kFALSE; }

      Int_t stl = -TClassEdit::IsSTLCont(GetName(), 0);
      if ((stl==ROOT::kSTLmap || stl==ROOT::kSTLmultimap)
          && !valueClass->HasDataMemberInfo()==0)
      {
         This->fCanSplit = 0;
         return kFALSE;
      }

      This->fCanSplit = 1;
      return kTRUE;

   }

   if (GetStreamer()!=0) {

      // We have an external custom streamer provided by the user, we must not
      // split it.
      This->fCanSplit = 0;
      return kFALSE;

   } else if ( TestBit(TClass::kHasCustomStreamerMember) ) {

      // We have a custom member function streamer or
      // an older (not StreamerInfo based) automatic streamer.
      This->fCanSplit = 0;
      return kFALSE;
   }

   if (Size()==1) {
      // 'Empty' class there is nothing to split!.
      This->fCanSplit = 0;
      return kFALSE;
   }

   // Calls to InheritsFrom for STL collection might cause unnecessary autoparsing.
   if (InheritsFrom("TRef"))      { This->fCanSplit = 0; return kFALSE; }
   if (InheritsFrom("TRefArray")) { This->fCanSplit = 0; return kFALSE; }
   if (InheritsFrom("TArray"))    { This->fCanSplit = 0; return kFALSE; }
   if (InheritsFrom("TCollection") && !InheritsFrom("TClonesArray")) { This->fCanSplit = 0; return kFALSE; }
   if (InheritsFrom("TTree"))     { This->fCanSplit = 0; return kFALSE; }

   TClass *ncThis = const_cast<TClass*>(this);
   TIter nextb(ncThis->GetListOfBases());
   TBaseClass *base;
   while((base = (TBaseClass*)nextb())) {
      if (!base->GetClassPointer()) { This->fCanSplit = 0; return kFALSE; }
   }

   This->fCanSplit = 1;
   return kTRUE;
}

//______________________________________________________________________________
Long_t TClass::ClassProperty() const
{
   // Return the C++ property of this class, eg. is abstract, has virtual base
   // class, see EClassProperty in TDictionary.h

   if (fProperty == -1) Property();
   return fClassProperty;
}

//______________________________________________________________________________
TObject *TClass::Clone(const char *new_name) const
{
   // Create a Clone of this TClass object using a different name but using the same 'dictionary'.
   // This effectively creates a hard alias for the class name.

   if (new_name == 0 || new_name[0]=='\0' || fName == new_name) {
      Error("Clone","The name of the class must be changed when cloning a TClass object.");
      return 0;
   }

   // Need to lock access to TROOT::GetListOfClasses so the cloning happens atomically
   R__LOCKGUARD2(gInterpreterMutex);
   // Temporarily remove the original from the list of classes.
   TClass::RemoveClass(const_cast<TClass*>(this));

   TClass *copy;
   if (fTypeInfo) {
      copy = new TClass(GetName(),
                        fClassVersion,
                        *fTypeInfo,
                        new TIsAProxy(*fTypeInfo),
                        GetDeclFileName(),
                        GetImplFileName(),
                        GetDeclFileLine(),
                        GetImplFileLine());
   } else {
      copy = new TClass(GetName(),
                        fClassVersion,
                        GetDeclFileName(),
                        GetImplFileName(),
                        GetDeclFileLine(),
                        GetImplFileLine());
   }
   copy->fShowMembers = fShowMembers;
   // Remove the copy before renaming it
   TClass::RemoveClass(copy);
   copy->fName = new_name;
   TClass::AddClass(copy);

   copy->SetNew(fNew);
   copy->SetNewArray(fNewArray);
   copy->SetDelete(fDelete);
   copy->SetDeleteArray(fDeleteArray);
   copy->SetDestructor(fDestructor);
   copy->SetDirectoryAutoAdd(fDirAutoAdd);
   copy->fStreamerFunc = fStreamerFunc;
   if (fStreamer) {
      copy->AdoptStreamer(fStreamer->Generate());
   }
   // If IsZombie is true, something went wrong and we will not be
   // able to properly copy the collection proxy
   if (fCollectionProxy && !copy->IsZombie()) {
      copy->CopyCollectionProxy(*fCollectionProxy);
   }
   copy->SetClassSize(fSizeof);
   if (fRefProxy) {
      copy->AdoptReferenceProxy( fRefProxy->Clone() );
   }
   TClass::AddClass(const_cast<TClass*>(this));
   return copy;
}

//______________________________________________________________________________
void TClass::CopyCollectionProxy(const TVirtualCollectionProxy &orig)
{
   // Copy the argument.

//     // This code was used too quickly test the STL Emulation layer
//    Int_t k = TClassEdit::IsSTLCont(GetName());
//    if (k==1||k==-1) return;

   delete fCollectionProxy;
   fCollectionProxy = orig.Generate();
}

//______________________________________________________________________________
void TClass::Draw(Option_t *option)
{
   // Draw detailed class inheritance structure.
   // If a class B inherits from a class A, the description of B is drawn
   // on the right side of the description of A.
   // Member functions overridden by B are shown in class A with a blue line
   // erasing the corresponding member function

   if (!HasInterpreterInfo()) return;

   TVirtualPad *padsav = gPad;

   // Should we create a new canvas?
   TString opt=option;
   if (!padsav || !opt.Contains("same")) {
      TVirtualPad *padclass = (TVirtualPad*)(gROOT->GetListOfCanvases())->FindObject("R__class");
      if (!padclass) {
         gROOT->ProcessLine("new TCanvas(\"R__class\",\"class\",20,20,1000,750);");
      } else {
         padclass->cd();
      }
   }

   if (gPad) gPad->DrawClassObject(this,option);

   if (padsav) padsav->cd();
}

//______________________________________________________________________________
void TClass::Dump(const void *obj, Bool_t noAddr /*=kFALSE*/) const
{
   // Dump contents of object on stdout.
   // Using the information in the object dictionary
   // each data member is interpreted.
   // If a data member is a pointer, the pointer value is printed
   // 'obj' is assume to point to an object of the class describe by this TClass
   //
   // The following output is the Dump of a TArrow object:
   //   fAngle                   0           Arrow opening angle (degrees)
   //   fArrowSize               0.2         Arrow Size
   //   fOption.*fData
   //   fX1                      0.1         X of 1st point
   //   fY1                      0.15        Y of 1st point
   //   fX2                      0.67        X of 2nd point
   //   fY2                      0.83        Y of 2nd point
   //   fUniqueID                0           object unique identifier
   //   fBits                    50331648    bit field status word
   //   fLineColor               1           line color
   //   fLineStyle               1           line style
   //   fLineWidth               1           line width
   //   fFillColor               19          fill area color
   //   fFillStyle               1001        fill area style
   //
   // If noAddr is true, printout of all pointer values is skipped.


   Long_t prObj = noAddr ? 0 : (Long_t)obj;
   if (IsTObject()) {
      if (!fIsOffsetStreamerSet) {
         CalculateStreamerOffset();
      }
      TObject *tobj = (TObject*)((Long_t)obj + fOffsetStreamer);


      if (sizeof(this) == 4)
         Printf("==> Dumping object at: 0x%08lx, name=%s, class=%s\n",prObj,tobj->GetName(),GetName());
      else
         Printf("==> Dumping object at: 0x%016lx, name=%s, class=%s\n",prObj,tobj->GetName(),GetName());
   } else {

      if (sizeof(this) == 4)
         Printf("==> Dumping object at: 0x%08lx, class=%s\n",prObj,GetName());
      else
         Printf("==> Dumping object at: 0x%016lx, class=%s\n",prObj,GetName());
   }

   TDumpMembers dm(noAddr);
   if (!CallShowMembers(obj, dm, kFALSE)) {
      Info("Dump", "No ShowMembers function, dumping disabled");
   }
}

//______________________________________________________________________________
char *TClass::EscapeChars(const char *text) const
{
   // Introduce an escape character (@) in front of a special chars.
   // You need to use the result immediately before it is being overwritten.

   static const UInt_t maxsize = 255;
   static char name[maxsize+2]; //One extra if last char needs to be escaped

   UInt_t nch = strlen(text);
   UInt_t icur = 0;
   for (UInt_t i = 0; i < nch && icur < maxsize; ++i, ++icur) {
      if (text[i] == '\"' || text[i] == '[' || text[i] == '~' ||
          text[i] == ']'  || text[i] == '&' || text[i] == '#' ||
          text[i] == '!'  || text[i] == '^' || text[i] == '<' ||
          text[i] == '?'  || text[i] == '>') {
         name[icur] = '@';
         ++icur;
      }
      name[icur] = text[i];
   }
   name[icur] = 0;
   return name;
}

//______________________________________________________________________________
TClass *TClass::GetActualClass(const void *object) const
{
   // Return a pointer the the real class of the object.
   // This is equivalent to object->IsA() when the class has a ClassDef.
   // It is REQUIRED that object is coming from a proper pointer to the
   // class represented by 'this'.
   // Example: Special case:
   //    class MyClass : public AnotherClass, public TObject
   // then on return, one must do:
   //    TObject *obj = (TObject*)((void*)myobject)directory->Get("some object of MyClass");
   //    MyClass::Class()->GetActualClass(obj); // this would be wrong!!!
   // Also if the class represented by 'this' and NONE of its parents classes
   // have a virtual ptr table, the result will be 'this' and NOT the actual
   // class.

   if (object==0) return (TClass*)this;
   if (!IsLoaded()) {
      TVirtualStreamerInfo* sinfo = GetStreamerInfo();
      if (sinfo) {
         return sinfo->GetActualClass(object);
      }
      return (TClass*)this;
   }
   if (fIsA) {
      return (*fIsA)(object); // ROOT::IsA((ThisClass*)object);
   } else if (fGlobalIsA) {
      return fGlobalIsA(this,object);
   } else {
      //Always call IsA via the interpreter. A direct call like
      //      object->IsA(brd, parent);
      //will not work if the class derives from TObject but not as primary
      //inheritance.
      if (fIsAMethod==0) {
         TMethodCall* temp = new TMethodCall((TClass*)this, "IsA", "");

         if (!temp->GetMethod()) {
            delete temp;
            Error("IsA","Can not find any IsA function for %s!",GetName());
            return (TClass*)this;
         }
         //Force cache to be updated here so do not have to worry about concurrency
         temp->ReturnType();

         TMethodCall* expected = nullptr;
         if( not fIsAMethod.compare_exchange_strong(expected,temp) ) {
            //another thread beat us to it
            delete temp;
         }
      }
      char * char_result = 0;
      (*fIsAMethod).Execute((void*)object, &char_result);
      return (TClass*)char_result;
   }
}

//______________________________________________________________________________
TClass *TClass::GetBaseClass(const char *classname)
{
   // Return pointer to the base class "classname". Returns 0 in case
   // "classname" is not a base class. Takes care of multiple inheritance.

   // check if class name itself is equal to classname
   if (strcmp(GetName(), classname) == 0) return this;

   if (!HasDataMemberInfo()) return 0;

   // Make sure we deal with possible aliases, we could also have normalized
   // the name.
   TClass *search = TClass::GetClass(classname,kTRUE,kTRUE);

   if (search) return GetBaseClass(search);
   else return 0;
}

//______________________________________________________________________________
TClass *TClass::GetBaseClass(const TClass *cl)
{
   // Return pointer to the base class "cl". Returns 0 in case "cl"
   // is not a base class. Takes care of multiple inheritance.

   // check if class name itself is equal to classname
   if (cl == this) return this;

   if (!HasDataMemberInfo()) return 0;

   TObjLink *lnk = GetListOfBases() ? fBase->FirstLink() : 0;

   // otherwise look at inheritance tree
   while (lnk) {
      TClass     *c, *c1;
      TBaseClass *base = (TBaseClass*) lnk->GetObject();
      c = base->GetClassPointer();
      if (c) {
         if (cl == c) return c;
         c1 = c->GetBaseClass(cl);
         if (c1) return c1;
      }
      lnk = lnk->Next();
   }
   return 0;
}

//______________________________________________________________________________
Int_t TClass::GetBaseClassOffsetRecurse(const TClass *cl)
{
   // Return data member offset to the base class "cl".
   // Returns -1 in case "cl" is not a base class.
   // Returns -2 if cl is a base class, but we can't find the offset
   // because it's virtual.
   // Takes care of multiple inheritance.

   // check if class name itself is equal to classname
   if (cl == this) return 0;

   if (!fBase) {
      if (fCanLoadClassInfo) LoadClassInfo();
      // If the information was not provided by the root pcm files and
      // if we can not find the ClassInfo, we have to fall back to the
      // StreamerInfo
      if (!fClassInfo) {
         TVirtualStreamerInfo *sinfo = GetCurrentStreamerInfo();
         if (!sinfo) return -1;
         TStreamerElement *element;
         Int_t offset = 0;

         TObjArray &elems = *(sinfo->GetElements());
         Int_t size = elems.GetLast()+1;
         for(Int_t i=0; i<size; i++) {
            element = (TStreamerElement*)elems[i];
            if (element->IsA() == TStreamerBase::Class()) {
               TStreamerBase *base = (TStreamerBase*)element;
               TClass *baseclass = base->GetClassPointer();
               if (!baseclass) return -1;
               Int_t subOffset = baseclass->GetBaseClassOffsetRecurse(cl);
               if (subOffset == -2) return -2;
               if (subOffset != -1) return offset+subOffset;
               offset += baseclass->Size();
            }
         }
         return -1;
      }
   }

   TClass     *c;
   Int_t      off;
   TBaseClass *inh;
   TObjLink *lnk = 0;
   if (fBase==0) lnk = GetListOfBases()->FirstLink();
   else lnk = fBase->FirstLink();

   // otherwise look at inheritance tree
   while (lnk) {
      inh = (TBaseClass *)lnk->GetObject();
      //use option load=kFALSE to avoid a warning like:
      //"Warning in <TClass::TClass>: no dictionary for class TRefCnt is available"
      //We can not afford to not have the class if it exist, so we
      //use kTRUE.
      c = inh->GetClassPointer(kTRUE); // kFALSE);
      if (c) {
         if (cl == c) {
            if ((inh->Property() & kIsVirtualBase) != 0)
               return -2;
            return inh->GetDelta();
         }
         off = c->GetBaseClassOffsetRecurse(cl);
         if (off == -2) return -2;
         if (off != -1) {
            return off + inh->GetDelta();
         }
      }
      lnk = lnk->Next();
   }
   return -1;
}

//______________________________________________________________________________
Int_t TClass::GetBaseClassOffset(const TClass *toBase, void *address, bool isDerivedObject)
{
   // Return data member offset to the base class "cl".
   // Returns -1 in case "cl" is not a base class.
   // Takes care of multiple inheritance.

   R__LOCKGUARD(gInterpreterMutex);
   // Warning("GetBaseClassOffset","Requires the use of fClassInfo for %s to %s",GetName(),toBase->GetName());

   if ((!address /* || !has_virtual_base */) &&
       (!HasInterpreterInfoInMemory() || !toBase->HasInterpreterInfoInMemory())) {
      // At least of the ClassInfo have not been loaded in memory yet and
      // since there is no virtual base class (or we don't have enough so it
      // would not make a difference) we can use the 'static' information
      Int_t offset = GetBaseClassOffsetRecurse (toBase);
      if (offset != -2) {
         return offset;
      }
      return offset;
   }

   ClassInfo_t* derived = GetClassInfo();
   ClassInfo_t* base = toBase->GetClassInfo();
   if(derived && base) {
      return gCling->ClassInfo_GetBaseOffset(derived, base, address, isDerivedObject);
   }
   else {
      Int_t offset = GetBaseClassOffsetRecurse (toBase);
      if (offset != -2) {
         return offset;
      }
   }
   return -1;
}

//______________________________________________________________________________
TClass *TClass::GetBaseDataMember(const char *datamember)
{
   // Return pointer to (base) class that contains datamember.

   if (!HasDataMemberInfo()) return 0;

   // Check if data member exists in class itself
   TDataMember *dm = GetDataMember(datamember);
   if (dm) return this;

   // if datamember not found in class, search in next base classes
   TBaseClass *inh;
   TIter       next(GetListOfBases());
   while ((inh = (TBaseClass *) next())) {
      TClass *c = inh->GetClassPointer();
      if (c) {
         TClass *cdm = c->GetBaseDataMember(datamember);
         if (cdm) return cdm;
      }
   }

   return 0;
}

namespace {
   // A local Helper class used to keep 2 pointer (the collection proxy
   // and the class streamer) in the thread local storage.

   struct TClassLocalStorage {
      TClassLocalStorage() : fCollectionProxy(0), fStreamer(0) {};

      TVirtualCollectionProxy *fCollectionProxy;
      TClassStreamer          *fStreamer;

      static TClassLocalStorage *GetStorage(const TClass *cl)
      {
         // Return the thread storage for the TClass.

         void **thread_ptr = (*gThreadTsd)(0,ROOT::kClassThreadSlot);
         if (thread_ptr) {
            if (*thread_ptr==0) *thread_ptr = new TExMap();
            TExMap *lmap = (TExMap*)(*thread_ptr);
            ULong_t hash = TString::Hash(&cl, sizeof(void*));
            ULong_t local = 0;
            UInt_t slot;
            if ((local = (ULong_t)lmap->GetValue(hash, (Long_t)cl, slot)) != 0) {
            } else {
               local = (ULong_t) new TClassLocalStorage();
               lmap->AddAt(slot, hash, (Long_t)cl, local);
            }
            return (TClassLocalStorage*)local;
         }
         return 0;
      }
   };
}

//______________________________________________________________________________
TVirtualCollectionProxy *TClass::GetCollectionProxy() const
{
   // Return the proxy describing the collection (if any).

   // Use assert, so that this line (slow because of the TClassEdit) is completely
   // removed in optimized code.
   assert(TestBit(kLoading) || !TClassEdit::IsSTLCont(fName) || fCollectionProxy || 0 == "The TClass for the STL collection has no collection proxy!");
   if (gThreadTsd && fCollectionProxy) {
      TClassLocalStorage *local = TClassLocalStorage::GetStorage(this);
      if (local == 0) return fCollectionProxy;
      if (local->fCollectionProxy==0) local->fCollectionProxy = fCollectionProxy->Generate();
      return local->fCollectionProxy;
   }
   return fCollectionProxy;
}

//______________________________________________________________________________
TClassStreamer *TClass::GetStreamer() const
{
   // Return the Streamer Class allowing streaming (if any).

   if (gThreadTsd && fStreamer) {
      TClassLocalStorage *local = TClassLocalStorage::GetStorage(this);
      if (local==0) return fStreamer;
      if (local->fStreamer==0) {
         local->fStreamer = fStreamer->Generate();
         const type_info &orig = ( typeid(*fStreamer) );
         if (!local->fStreamer) {
            Warning("GetStreamer","For %s, the TClassStreamer (%s) passed's call to Generate failed!",GetName(),orig.name());
         } else {
            const type_info &copy = ( typeid(*local->fStreamer) );
            if (strcmp(orig.name(),copy.name())!=0) {
               Warning("GetStreamer","For %s, the TClassStreamer passed does not properly implement the Generate method (%s vs %s)\n",GetName(),orig.name(),copy.name());
            }
         }
      }
      return local->fStreamer;
   }
   return fStreamer;
}
//______________________________________________________________________________
ClassStreamerFunc_t TClass::GetStreamerFunc() const
{
   // Get a wrapper/accessor function around this class custom streamer (member function).

   return fStreamerFunc;
}

//______________________________________________________________________________
TVirtualIsAProxy* TClass::GetIsAProxy() const
{
   // Return the proxy implementing the IsA functionality.

   return fIsA;
}

//______________________________________________________________________________
TClass *TClass::GetClass(const char *name, Bool_t load, Bool_t silent)
{
   // Static method returning pointer to TClass of the specified class name.
   // If load is true an attempt is made to obtain the class by loading
   // the appropriate shared library (directed by the rootmap file).
   // If silent is 'true', do not warn about missing dictionary for the class.
   // (typically used for class that are used only for transient members)
   // Returns 0 in case class is not found.

   if (!name || !name[0]) return 0;

   if (strstr(name, "(anonymous)")) return 0;
   if (strncmp(name,"class ",6)==0) name += 6;
   if (strncmp(name,"struct ",7)==0) name += 7;

   R__LOCKGUARD(gInterpreterMutex);

   if (!gROOT->GetListOfClasses())  return 0;

   TClass *cl = (TClass*)gROOT->GetListOfClasses()->FindObject(name);

   // Early return to release the lock without having to execute the
   // long-ish normalization.
   if (cl) {
      if (cl->IsLoaded() || cl->TestBit(kUnloading)) return cl;

      // We could speed-up some of the search by adding (the equivalent of)
      //
      //    if (cl->GetState() == kInterpreter) return cl
      //
      // In this case, if a ROOT dictionary was available when the TClass
      // was first request it would have been used and if a ROOT dictonary is
      // loaded later on TClassTable::Add will take care of updating the TClass.
      // So as far as ROOT dictionary are concerned, if the current TClass is
      // in interpreted state, we are sure there is nothing to load.
      //
      // However (see TROOT::LoadClass), the TClass can also be loaded/provided
      // by a user provided TClassGenerator.  We have no way of knowing whether
      // those do (or even can) behave the same way as the ROOT dictionary and
      // have the 'dictionary is now available for use' step informa the existing
      // TClass that their dictionary is now available.

      //we may pass here in case of a dummy class created by TVirtualStreamerInfo
      load = kTRUE;
   }

   // To avoid spurrious auto parsing, let's check if the name as-is is
   // known in the TClassTable.
   DictFuncPtr_t dict = TClassTable::GetDictNorm(name);
   if (dict) {
      // The name is normalized, so the result of the first search is
      // authoritative.
      if (!cl && !load) return 0;

      TClass *loadedcl = (dict)();
      if (loadedcl) {
         loadedcl->PostLoadCheck();
         return loadedcl;
      }

      // We should really not fall through to here, but if we do, let's just
      // continue as before ...
   }

   std::string normalizedName;
   Bool_t checkTable = kFALSE;

   if (!cl) {
      int oldAutoloadVal = gCling->SetClassAutoloading(false);
      TClassEdit::GetNormalizedName(normalizedName, name);
      gCling->SetClassAutoloading(oldAutoloadVal);
      // Try the normalized name.
      if (normalizedName != name) {
         cl = (TClass*)gROOT->GetListOfClasses()->FindObject(normalizedName.c_str());

         if (cl) {
            if (cl->IsLoaded() || cl->TestBit(kUnloading)) return cl;

            //we may pass here in case of a dummy class created by TVirtualStreamerInfo
            load = kTRUE;
         }
         checkTable = kTRUE;
     }
   } else {
      normalizedName = cl->GetName(); // Use the fact that all TClass names are normalized.
      checkTable = load && (normalizedName != name);
   }

   if (!load) return 0;

// This assertion currently fails because of
//   TClass *c1 = TClass::GetClass("basic_iostream<char,char_traits<char> >");
//   TClass *c2 = TClass::GetClass("std::iostream");
// where the TClassEdit normalized name of iostream is basic_iostream<char>
// i.e missing the addition of the default parameter.  This is because TClingLookupHelper
// uses only 'part' of TMetaUtils::GetNormalizedName.

//   if (!cl) {
//      TDataType* dataType = (TDataType*)gROOT->GetListOfTypes()->FindObject(name);
//      TClass *altcl = dataType ? (TClass*)gROOT->GetListOfClasses()->FindObject(dataType->GetFullTypeName()) : 0;
//      if (altcl && normalizedName != altcl->GetName())
//         ::Fatal("TClass::GetClass","The existing name (%s) for %s is different from the normalized name: %s\n",
//                 altcl->GetName(), name, normalizedName.c_str());
//   }

   TClass *loadedcl = 0;
   if (checkTable) {
      loadedcl = LoadClassDefault(normalizedName.c_str(),silent);
   } else {
      if (gInterpreter->AutoLoad(normalizedName.c_str(),kTRUE)) {
         loadedcl = LoadClassDefault(normalizedName.c_str(),silent);
      }
      // Maybe this was a typedef: let's try to see if this is the case
      if (!loadedcl){
         if (TDataType* theDataType = gROOT->GetType(normalizedName.c_str())){
            // We have a typedef: we get the name of the underlying type
            auto underlyingTypeName = theDataType->GetTypeName().Data();
            // We see if we can bootstrap a class with it
            auto underlyingTypeDict = TClassTable::GetDictNorm(underlyingTypeName);
            if (underlyingTypeDict){
               loadedcl = underlyingTypeDict();
            }

         }
      }
   }
   if (loadedcl) return loadedcl;

   // See if the TClassGenerator can produce the TClass we need.
   loadedcl = LoadClassCustom(normalizedName.c_str(),silent);
   if (loadedcl) return loadedcl;

   // We have not been able to find a loaded TClass, return the Emulated
   // TClass if we have one.
   if (cl) return cl;

   if (TClassEdit::IsSTLCont( normalizedName.c_str() )) {

      return gInterpreter->GenerateTClass(normalizedName.c_str(), kTRUE, silent);
   }

   // Check the interpreter only after autoparsing the template if any.
   {
      std::string::size_type posLess = normalizedName.find('<');
      if (posLess != std::string::npos) {
         gCling->AutoParse(normalizedName.substr(0, posLess).c_str());
      }
   }

   //last attempt. Look in CINT list of all (compiled+interpreted) classes
   if (gDebug>0){
      printf("TClass::GetClass: Header Parsing - The representation of %s was not found in the type system. A lookup in the interpreter is about to be tried: this can cause parsing. This can be avoided selecting %s in the linkdef/selection file.\n",normalizedName.c_str(), normalizedName.c_str());
   }
   if (gInterpreter->CheckClassInfo(normalizedName.c_str(), kTRUE /* autoload */, kTRUE /*Only class, structs and ns*/)) {
      // Get the normalized name based on the decl (currently the only way
      // to get the part to add or drop the default arguments as requested by the user)
      std::string alternative;
      gInterpreter->GetInterpreterTypeName(normalizedName.c_str(),alternative,kTRUE);
      const char *altname = alternative.c_str();
      if ( strncmp(altname,"std::",5)==0 ) {
         // For namespace (for example std::__1), GetInterpreterTypeName does
         // not strip std::, so we must do it explicitly here.
         altname += 5;
      }
      if (altname != normalizedName && strcmp(altname,name) != 0) {
         // altname now contains the full name of the class including a possible
         // namespace if there has been a using namespace statement.

         // At least in the case C<string [2]> (normalized) vs C<string[2]> (altname)
         // the TClassEdit normalization and the TMetaUtils normalization leads to
         // two different space layout.  To avoid an infinite recursion, we also
         // add the test on (altname != name)

         return GetClass(altname,load);
      }
      TClass *ncl = gInterpreter->GenerateTClass(normalizedName.c_str(), /* emulation = */ kFALSE, silent);
      if (!ncl->IsZombie()) {
         return ncl;
      }
      delete ncl;
   }
   return 0;
}

//______________________________________________________________________________
TClass *TClass::GetClass(const type_info& typeinfo, Bool_t load, Bool_t /* silent */)
{
   // Return pointer to class with name.

   //protect access to TROOT::GetListOfClasses
   R__LOCKGUARD2(gInterpreterMutex);

   if (!gROOT->GetListOfClasses())    return 0;

   TClass* cl = GetIdMap()->Find(typeinfo.name());

   if (cl) {
      if (cl->IsLoaded()) return cl;
      //we may pass here in case of a dummy class created by TVirtualStreamerInfo
      load = kTRUE;
   } else {
     // Note we might need support for typedefs and simple types!

     //      TDataType *objType = GetType(name, load);
     //if (objType) {
     //    const char *typdfName = objType->GetTypeName();
     //    if (typdfName && strcmp(typdfName, name)) {
     //       cl = GetClass(typdfName, load);
     //       return cl;
     //    }
     // }
   }

   if (!load) return 0;

   DictFuncPtr_t dict = TClassTable::GetDict(typeinfo);
   if (dict) {
      cl = (dict)();
      if (cl) cl->PostLoadCheck();
      return cl;
   }
   if (cl) return cl;

   TIter next(gROOT->GetListOfClassGenerators());
   TClassGenerator *gen;
   while( (gen = (TClassGenerator*) next()) ) {
      cl = gen->GetClass(typeinfo,load);
      if (cl) {
         cl->PostLoadCheck();
         return cl;
      }
   }

   // try autoloading the typeinfo
   int autoload_old = gCling->SetClassAutoloading(1);
   if (!autoload_old) {
      // Re-disable, we just meant to test
      gCling->SetClassAutoloading(0);
   }
   if (autoload_old && gInterpreter->AutoLoad(typeinfo,kTRUE)) {
      // Disable autoload to avoid potential infinite recursion
      gCling->SetClassAutoloading(0);
      cl = GetClass(typeinfo, load);
      gCling->SetClassAutoloading(1);
      if (cl) {
         return cl;
      }
   }

   // last attempt. Look in the interpreter list of all (compiled+interpreted)
   // classes
   cl = gInterpreter->GetClass(typeinfo, load);

   return cl; // Can be zero.
}

//______________________________________________________________________________
TClass *TClass::GetClass(ClassInfo_t *info, Bool_t load, Bool_t silent)
{
   // Static method returning pointer to TClass of the specified ClassInfo.
   // If load is true an attempt is made to obtain the class by loading
   // the appropriate shared library (directed by the rootmap file).
   // If silent is 'true', do not warn about missing dictionary for the class.
   // (typically used for class that are used only for transient members)
   // Returns 0 in case class is not found.

   if (!info || !gCling->ClassInfo_IsValid(info)) return 0;
   if (!gROOT->GetListOfClasses())    return 0;

   // Get the normalized name.
   TString name( gCling->ClassInfo_FullName(info) );

   TClass *cl = (TClass*)gROOT->GetListOfClasses()->FindObject(name);

   if (cl) {
      if (cl->IsLoaded()) return cl;

      //we may pass here in case of a dummy class created by TVirtualStreamerInfo
      load = kTRUE;

   }

   if (!load) return 0;

   TClass *loadedcl = 0;
   if (cl) loadedcl = gROOT->LoadClass(cl->GetName(),silent);
   else    loadedcl = gROOT->LoadClass(name,silent);

   if (loadedcl) return loadedcl;

   if (cl) return cl;  // If we found the class but we already have a dummy class use it.

   // We did not find a proper TClass but we do know (we have a valid
   // ClassInfo) that the class is known to the interpreter.
   TClass *ncl = gInterpreter->GenerateTClass(info, silent);
   if (!ncl->IsZombie()) {
      return ncl;
   } else {
      delete ncl;
      return 0;
   }
}

//______________________________________________________________________________
Bool_t TClass::HasNoInfoOrEmuOrFwdDeclaredDecl(const char* name){
   return fNoInfoOrEmuOrFwdDeclNameRegistry.HasDeclName(name);
}

//______________________________________________________________________________
Bool_t TClass::GetClass(DeclId_t id, std::vector<TClass*> &classes)
{

   if (!gROOT->GetListOfClasses())    return 0;

   DeclIdMap_t* map = GetDeclIdMap();
   // Get all the TClass pointer that have the same DeclId.
   DeclIdMap_t::equal_range iter = map->Find(id);
   if (iter.first == iter.second) return false;
   std::vector<TClass*>::iterator vectIt = classes.begin();
   for (DeclIdMap_t::const_iterator it = iter.first; it != iter.second; ++it)
      vectIt = classes.insert(vectIt, it->second);
   return true;
}

//______________________________________________________________________________
DictFuncPtr_t  TClass::GetDict (const char *cname)
{
   // Return a pointer to the dictionary loading function generated by
   // rootcint

   return TClassTable::GetDict(cname);
}

//______________________________________________________________________________
DictFuncPtr_t  TClass::GetDict (const type_info& info)
{
   // Return a pointer to the dictionary loading function generated by
   // rootcint

   return TClassTable::GetDict(info);
}

//______________________________________________________________________________
TDataMember *TClass::GetDataMember(const char *datamember) const
{
   // Return pointer to datamember object with name "datamember".

   if ((!(fData && fData->IsLoaded()) && !HasInterpreterInfo())
       || datamember == 0) return 0;

   // Strip off leading *'s and trailing [
   const char *start_name = datamember;
   while (*start_name == '*') ++start_name;

   // Empty name are 'legal', they represent anonymous unions.
   //   if (*start_name == 0) return 0;

   if (const char *s = strchr(start_name, '[')){
      UInt_t len = s-start_name;
      TString name(start_name,len);
      return (TDataMember *)((TClass*)this)->GetListOfDataMembers(kFALSE)->FindObject(name.Data());
   } else {
      return (TDataMember *)((TClass*)this)->GetListOfDataMembers(kFALSE)->FindObject(start_name);
   }
}

//______________________________________________________________________________
Long_t TClass::GetDataMemberOffset(const char *name) const
{
   // return offset for member name. name can be a data member in
   // the class itself, one of its base classes, or one member in
   // one of the aggregated classes.
   //
   // In case of an emulated class, the list of emulated TRealData is built

   TRealData *rd = GetRealData(name);
   if (rd) return rd->GetThisOffset();
   if (strchr(name,'[')==0) {
      // If this is a simple name there is a chance to find it in the
      // StreamerInfo even if we did not find it in the RealData.
      // For example an array name would be fArray[3] in RealData but
      // just fArray in the streamerInfo.
      TVirtualStreamerInfo *info = const_cast<TClass*>(this)->GetCurrentStreamerInfo();
      if (info) {
         return info->GetOffset(name);
      }
   }
   return 0;
}

//______________________________________________________________________________
TRealData* TClass::GetRealData(const char* name) const
{
   // -- Return pointer to TRealData element with name "name".
   //
   // Name can be a data member in the class itself,
   // one of its base classes, or a member in
   // one of the aggregated classes.
   //
   // In case of an emulated class, the list of emulated TRealData is built.
   //

   if (!fRealData) {
      const_cast<TClass*>(this)->BuildRealData();
   }

   if (!fRealData) {
      return 0;
   }

   if (!name) {
      return 0;
   }

   // First try just the whole name.
   TRealData* rd = (TRealData*) fRealData->FindObject(name);
   if (rd) {
      return rd;
   }

   std::string givenName(name);

   // Try ignoring the array dimensions.
   std::string::size_type firstBracket = givenName.find_first_of("[");
   if (firstBracket != std::string::npos) {
      // -- We are looking for an array data member.
      std::string nameNoDim(givenName.substr(0, firstBracket));
      TObjLink* lnk = fRealData->FirstLink();
      while (lnk) {
         TObject* obj = lnk->GetObject();
         std::string objName(obj->GetName());
         std::string::size_type pos = objName.find_first_of("[");
         // Only match arrays to arrays for now.
         if (pos != std::string::npos) {
            objName.erase(pos);
            if (objName == nameNoDim) {
               return static_cast<TRealData*>(obj);
            }
         }
         lnk = lnk->Next();
      }
   }

   // Now try it as a pointer.
   std::ostringstream ptrname;
   ptrname << "*" << givenName;
   rd = (TRealData*) fRealData->FindObject(ptrname.str().c_str());
   if (rd) {
      return rd;
   }

   // Check for a dot in the name.
   std::string::size_type firstDot = givenName.find_first_of(".");
   if (firstDot == std::string::npos) {
      // -- Not found, a simple name, all done.
      return 0;
   }

   //
   //  At this point the name has a dot in it, so it is the name
   //  of some contained sub-object.
   //

   // May be a pointer like in TH1: fXaxis.fLabels (in TRealdata is named fXaxis.*fLabels)
   std::string::size_type lastDot = givenName.find_last_of(".");
   std::ostringstream starname;
   starname << givenName.substr(0, lastDot) << ".*" << givenName.substr(lastDot + 1);
   rd = (TRealData*) fRealData->FindObject(starname.str().c_str());
   if (rd) {
      return rd;
   }

   // Strip the first component, it may be the name of
   // the branch (old TBranchElement code), and try again.
   std::string firstDotName(givenName.substr(firstDot + 1));

   // New attempt starting after the first "." if any,
   // this allows for the case that the first component
   // may have been a branch name (for TBranchElement).
   rd = (TRealData*) fRealData->FindObject(firstDotName.c_str());
   if (rd) {
      return rd;
   }

   // New attempt starting after the first "." if any,
   // but this time try ignoring the array dimensions.
   // Again, we are allowing for the case that the first
   // component may have been a branch name (for TBranchElement).
   std::string::size_type firstDotBracket = firstDotName.find_first_of("[");
   if (firstDotBracket != std::string::npos) {
      // -- We are looking for an array data member.
      std::string nameNoDim(firstDotName.substr(0, firstDotBracket));
      TObjLink* lnk = fRealData->FirstLink();
      while (lnk) {
         TObject* obj = lnk->GetObject();
         std::string objName(obj->GetName());
         std::string::size_type pos = objName.find_first_of("[");
         // Only match arrays to arrays for now.
         if (pos != std::string::npos) {
            objName.erase(pos);
            if (objName == nameNoDim) {
               return static_cast<TRealData*>(obj);
            }
         }
         lnk = lnk->Next();
      }
   }

   // New attempt starting after the first "." if any,
   // but this time check for a pointer type.  Again, we
   // are allowing for the case that the first component
   // may have been a branch name (for TBranchElement).
   ptrname.str("");
   ptrname << "*" << firstDotName;
   rd = (TRealData*) fRealData->FindObject(ptrname.str().c_str());
   if (rd) {
      return rd;
   }

   // Last attempt in case a member has been changed from
   // a static array to a pointer, for example the member
   // was arr[20] and is now *arr.
   //
   // Note: In principle, one could also take into account
   // the opposite situation where a member like *arr has
   // been converted to arr[20].
   //
   // FIXME: What about checking after the first dot as well?
   //
   std::string::size_type bracket = starname.str().find_first_of("[");
   if (bracket == std::string::npos) {
      return 0;
   }
   rd = (TRealData*) fRealData->FindObject(starname.str().substr(0, bracket).c_str());
   if (rd) {
      return rd;
   }

   // Not found;
   return 0;
}

//______________________________________________________________________________
TFunctionTemplate *TClass::GetFunctionTemplate(const char *name)
{
   if (!gInterpreter || !HasInterpreterInfo()) return 0;

   // The following
   if (!fFuncTemplate) fFuncTemplate = new TListOfFunctionTemplates(this);

   return (TFunctionTemplate*)fFuncTemplate->FindObject(name);
}

//______________________________________________________________________________
const char *TClass::GetSharedLibs()
{
   // Get the list of shared libraries containing the code for class cls.
   // The first library in the list is the one containing the class, the
   // others are the libraries the first one depends on. Returns 0
   // in case the library is not found.

   if (!gInterpreter) return 0;

   if (fSharedLibs.IsNull())
      fSharedLibs = gInterpreter->GetClassSharedLibs(fName);

   return !fSharedLibs.IsNull() ? fSharedLibs.Data() : 0;
}

//______________________________________________________________________________
TList *TClass::GetListOfBases()
{
   // Return list containing the TBaseClass(es) of a class.

   if (!fBase) {
      if (fCanLoadClassInfo) {
         if (fState == kHasTClassInit) {

            R__LOCKGUARD(gInterpreterMutex);
            // NOTE: Add test to prevent redo if another thread has already done the work.
            // if (!fHasRootPcmInfo) {

            // The bases are in our ProtoClass; we don't need the class info.
            TProtoClass *proto = TClassTable::GetProtoNorm(GetName());
            if (proto && proto->FillTClass(this)) {
               // Not sure this code is still needed
               // R__ASSERT(kFALSE);

               fHasRootPcmInfo = kTRUE;
            }
         }
         // We test again on fCanLoadClassInfo has another thread may have executed it.
         if (!fHasRootPcmInfo && !fCanLoadClassInfo) {
            LoadClassInfo();
         }
      }
      if (!fClassInfo) return 0;

      if (!gInterpreter)
         Fatal("GetListOfBases", "gInterpreter not initialized");

      R__LOCKGUARD(gInterpreterMutex);
      if(!fBase) {
         gInterpreter->CreateListOfBaseClasses(this);
      }
   }
   return fBase;
}

//______________________________________________________________________________
TList *TClass::GetListOfEnums(Bool_t load /* = kTRUE */)
{
   // Return list containing the TEnums of a class.

   R__LOCKGUARD(gInterpreterMutex);

   if (!fEnums) fEnums = new TListOfEnums(this);
   if (load) fEnums->Load();
   return fEnums;
}

//______________________________________________________________________________
TList *TClass::GetListOfDataMembers(Bool_t load /* = kTRUE */)
{
   // Return list containing the TDataMembers of a class.

   R__LOCKGUARD(gInterpreterMutex);

   if (!fData) {
      if (fCanLoadClassInfo && fState == kHasTClassInit) {
         // NOTE: Add test to prevent redo if another thread has already done the work.
         // if (!fHasRootPcmInfo) {

         // The members are in our ProtoClass; we don't need the class info.
         TProtoClass *proto = TClassTable::GetProtoNorm(GetName());
         if (proto && proto->FillTClass(this)) {
            // Not sure this code is still needed
            // R__ASSERT(kFALSE);

            fHasRootPcmInfo = kTRUE;
            return fData;
         }
      }
      fData = new TListOfDataMembers(this);
   }
   if (Property() & (kIsClass|kIsStruct|kIsUnion)) {
      // If the we have a class or struct or union, the order
      // of data members is the list is essential since it determines their
      // order on file.  So we must always load.  Also, the list is fixed
      // since the language does not allow to add members.
      if (!fData->IsLoaded()) fData->Load();

   } else if (load) fData->Load();
   return fData;
}

//______________________________________________________________________________
TList *TClass::GetListOfFunctionTemplates(Bool_t load /* = kTRUE */)
{
   // Return list containing the TEnums of a class.

   R__LOCKGUARD(gInterpreterMutex);

   if (!fFuncTemplate) fFuncTemplate = new TListOfFunctionTemplates(this);
   if (load) fFuncTemplate->Load();
   return fFuncTemplate;
}

//______________________________________________________________________________
TList *TClass::GetListOfMethods(Bool_t load /* = kTRUE */)
{
   // Return list containing the TMethods of a class.
   // If load is true, the list is populated with all the defined function
   // and currently instantiated function template.

   R__LOCKGUARD(gInterpreterMutex);

   if (!fMethod) fMethod = new TListOfFunctions(this);
   if (load) {
      if (gDebug>0) Info("GetListOfMethods","Header Parsing - Asking for all the methods of class %s: this can involve parsing.",GetName());
      fMethod->Load();
   }
   return fMethod;
}

//______________________________________________________________________________
TCollection *TClass::GetListOfMethodOverloads(const char* name) const
{
   // Return the collection of functions named "name".
   return ((TListOfFunctions*)fMethod)->GetListForObject(name);
}


//______________________________________________________________________________
const TList *TClass::GetListOfAllPublicMethods(Bool_t load /* = kTRUE */)
{
   // Returns a list of all public methods of this class and its base classes.
   // Refers to a subset of the methods in GetListOfMethods() so don't do
   // GetListOfAllPublicMethods()->Delete().
   // Algorithm used to get the list is:
   // - put all methods of the class in the list (also protected and private
   //   ones).
   // - loop over all base classes and add only those methods not already in the
   //   list (also protected and private ones).
   // - once finished, loop over resulting list and remove all private and
   //   protected methods.

   R__LOCKGUARD(gInterpreterMutex);

   if (!fAllPubMethod) fAllPubMethod = new TViewPubFunctions(this);
   if (load) {
      if (gDebug>0) Info("GetListOfAllPublicMethods","Header Parsing - Asking for all the methods of class %s: this can involve parsing.",GetName());
      fAllPubMethod->Load();
   }
   return fAllPubMethod;
}

//______________________________________________________________________________
TList *TClass::GetListOfAllPublicDataMembers(Bool_t load /* = kTRUE */)
{
   // Returns a list of all public data members of this class and its base
   // classes. Refers to a subset of the data members in GetListOfDatamembers()
   // so don't do GetListOfAllPublicDataMembers()->Delete().

   R__LOCKGUARD(gInterpreterMutex);

   if (!fAllPubData) fAllPubData = new TViewPubDataMembers(this);
   if (load) fAllPubData->Load();
   return fAllPubData;
}

//______________________________________________________________________________
void TClass::GetMenuItems(TList *list)
{
   // Returns list of methods accessible by context menu.

   if (!HasInterpreterInfo()) return;

   // get the base class
   TIter nextBase(GetListOfBases(), kIterBackward);
   TBaseClass *baseClass;
   while ((baseClass = (TBaseClass *) nextBase())) {
      TClass *base = baseClass->GetClassPointer();
      if (base) base->GetMenuItems(list);
   }

   // remove methods redefined in this class with no menu
   TMethod *method, *m;
   TIter next(GetListOfMethods(), kIterBackward);
   while ((method = (TMethod*)next())) {
      m = (TMethod*)list->FindObject(method->GetName());
      if (method->IsMenuItem() != kMenuNoMenu) {
         if (!m)
            list->AddFirst(method);
      } else {
         if (m && m->GetNargs() == method->GetNargs())
            list->Remove(m);
      }
   }
}

//______________________________________________________________________________
Bool_t TClass::HasDictionary()
{
   // Check whether a class has a dictionary or not.
   // This is equivalent to ask if a class is coming from a bootstrapping
   // procedure initiated during the loading of a library.

   return IsLoaded();
}

//______________________________________________________________________________
Bool_t TClass::HasDictionarySelection(const char* clname)
{
   // Check whether a class has a dictionary or ROOT can load one.
   // This is equivalent to ask HasDictionary() or whether a library is known
   // where it can be loaded from, or whether a Dictionary function is
   // available because the class's dictionary library was already loaded.

   if (TClass* cl = (TClass*)gROOT->GetListOfClasses()->FindObject(clname))
      return cl->IsLoaded();
   return  gClassTable->GetDict(clname) || gInterpreter->GetClassSharedLibs(clname);
}

//______________________________________________________________________________
void TClass::GetMissingDictionariesForBaseClasses(TCollection& result, TCollection& visited, bool recurse)
{
   // Verify the base classes always.

   TList* lb = GetListOfBases();
   if (!lb) return;
   TIter nextBase(lb);
   TBaseClass* base = 0;
   while ((base = (TBaseClass*)nextBase())) {
      TClass* baseCl = base->Class();
      if (baseCl) {
            baseCl->GetMissingDictionariesWithRecursionCheck(result, visited, recurse);
      }
   }
}

//______________________________________________________________________________
void TClass::GetMissingDictionariesForMembers(TCollection& result, TCollection& visited, bool recurse)
{
   // Verify the Data Members.

   TListOfDataMembers* ldm = (TListOfDataMembers*)GetListOfDataMembers();
   if (!ldm) return ;
   TIter nextMemb(ldm);
   TDataMember * dm = 0;
   while ((dm = (TDataMember*)nextMemb())) {
      // If it is a transient
      if(!dm->IsPersistent()) {
        continue;
      }
      if (dm->Property() & kIsStatic) {
         continue;
      }
      // If it is a built-in data type.
      TClass* dmTClass = 0;
      if (dm->GetDataType()) {
         dmTClass = dm->GetDataType()->Class();
         // Otherwise get the string representing the type.
      } else if (dm->GetTypeName()) {
            dmTClass = TClass::GetClass(dm->GetTypeName());
      }
      if (dmTClass) {
            dmTClass->GetMissingDictionariesWithRecursionCheck(result, visited, recurse);
      }
   }
}

void TClass::GetMissingDictionariesForPairElements(TCollection& result, TCollection& visited, bool recurse)
{
   // Pair is a special case and we have to check its elements for missing dictionaries
   // Pair is a transparent container so we should always look at its.

   TVirtualStreamerInfo *SI = (TVirtualStreamerInfo*)this->GetStreamerInfo();
   for (int i = 0; i < 2; i++) {
      TClass* pairElement = ((TStreamerElement*)SI->GetElements()->At(i))->GetClass();
      if (pairElement) {
         pairElement->GetMissingDictionariesWithRecursionCheck(result, visited, recurse);
      }
   }
}

//______________________________________________________________________________
void TClass::GetMissingDictionariesWithRecursionCheck(TCollection& result, TCollection& visited, bool recurse)
{
   // From the second level of recursion onwards it is different state check.

   if (result.FindObject(this) || visited.FindObject(this)) return;

   static TClassRef sCIString("string");
   if (this == sCIString) return;

   // Special treatment for pair.
   if (strncmp(fName, "pair<", 5) == 0) {
      GetMissingDictionariesForPairElements(result, visited, recurse);
      return;
   }

   if (!HasDictionary()) {
      result.Add(this);
   }

   visited.Add(this);
   //Check whether a custom streamer
   if (!TestBit(TClass::kHasCustomStreamerMember)) {
      if (GetCollectionProxy()) {
         // We need to look at the collection's content
         // The collection has different kind of elements the check would be required.
         TClass* t = 0;
         if ((t = GetCollectionProxy()->GetValueClass())) {
            if (!t->HasDictionary()) {
               t->GetMissingDictionariesWithRecursionCheck(result, visited, recurse);
            }
         }
      } else {
         if (recurse) {
            GetMissingDictionariesForMembers(result, visited, recurse);
         }
         GetMissingDictionariesForBaseClasses(result, visited, recurse);
      }
   }
}

//______________________________________________________________________________
void TClass::GetMissingDictionaries(THashTable& result, bool recurse)
{
   // Get the classes that have a missing dictionary starting from this one.
   // With recurse = false the classes checked for missing dictionaries are:
   //                      the class itself, all base classes, direct data members,
   //                      and for collection proxies the container's
   //                      elements without iterating over the element's data members;
   // With recurse = true the classes checked for missing dictionaries are:
   //                      the class itself, all base classes, recursing on the data members,
   //                      and for the collection proxies recursiong on the elements of the
   //                      collection and iterating over the element's data members.

   // Top level recursion it different from the following levels of recursion.

   if (result.FindObject(this)) return;

   static TClassRef sCIString("string");
   if (this == sCIString) return;

   THashTable visited;

   if (strncmp(fName, "pair<", 5) == 0) {
      GetMissingDictionariesForPairElements(result, visited, recurse);
      return;
   }

   if (!HasDictionary()) {
      result.Add(this);
   }

   visited.Add(this);

   //Check whether a custom streamer
   if (!TestBit(TClass::kHasCustomStreamerMember)) {
      if (GetCollectionProxy()) {
         // We need to look at the collection's content
         // The collection has different kind of elements the check would be required.
         TClass* t = 0;
         if ((t = GetCollectionProxy()->GetValueClass())) {
            if (!t->HasDictionary()) {
               t->GetMissingDictionariesWithRecursionCheck(result, visited, recurse);
            }
         }
      } else {
         GetMissingDictionariesForMembers(result, visited, recurse);
         GetMissingDictionariesForBaseClasses(result, visited, recurse);
      }
   }
}

//______________________________________________________________________________
Bool_t TClass::IsFolder(void *obj) const
{
   // Return kTRUE if the class has elements.

   return Browse(obj,(TBrowser*)0);
}

//______________________________________________________________________________
//______________________________________________________________________________
void TClass::ReplaceWith(TClass *newcl) const
{
   // Inform the other objects to replace this object by the new TClass (newcl)

   R__LOCKGUARD(gInterpreterMutex);
   //we must update the class pointers pointing to 'this' in all TStreamerElements
   TIter nextClass(gROOT->GetListOfClasses());
   TClass *acl;
   TVirtualStreamerInfo *info;
   TList tobedeleted;

   // Since we are in the process of replacing a TClass by a TClass
   // coming from a dictionary, there is no point in loading any
   // libraries during this search.
   Bool_t autoload = gInterpreter->SetClassAutoloading(kFALSE);

   while ((acl = (TClass*)nextClass())) {
      if (acl == newcl) continue;

      TIter nextInfo(acl->GetStreamerInfos());
      while ((info = (TVirtualStreamerInfo*)nextInfo())) {

         info->Update(this, newcl);
      }

      if (acl->GetCollectionProxy()) {
         acl->GetCollectionProxy()->UpdateValueClass(this, newcl);
      }
      // We should also inform all the TBranchElement :( but we do not have a master list :(
   }

   TIter delIter( &tobedeleted );
   while ((acl = (TClass*)delIter())) {
      delete acl;
   }
   gInterpreter->UnRegisterTClassUpdate(this);

   gInterpreter->SetClassAutoloading(autoload);
}

//______________________________________________________________________________
void TClass::ResetClassInfo(Long_t /* tagnum */)
{
   // Make sure that the current ClassInfo is up to date.

   Warning("ResetClassInfo(Long_t tagnum)","Call to deprecated interface (does nothing)");
}

//______________________________________________________________________________
void TClass::ResetClassInfo()
{
   // Make sure that the current ClassInfo is up to date.
   R__LOCKGUARD2(gInterpreterMutex);

   InsertTClassInRegistryRAII insertRAII(fState,fName,fNoInfoOrEmuOrFwdDeclNameRegistry);

   if (fClassInfo) {
      TClass::RemoveClassDeclId(gInterpreter->GetDeclId(fClassInfo));
      gInterpreter->ClassInfo_Delete(fClassInfo);
      fClassInfo = 0;
   }
   // We can not check at this point whether after the unload there will
   // still be interpreter information about this class (as v5 was doing),
   // instead this function must only be called if the definition is (about)
   // to be unloaded.

   ResetCaches();

   // We got here because the definition Decl is about to be unloaded.
   if (fState != TClass::kHasTClassInit) {
      if (fStreamerInfo->GetEntries() != 0) {
         fState = TClass::kEmulated;
      } else {
         fState = TClass::kForwardDeclared;
      }
   } else {
      // if the ClassInfo was loaded for a class with a TClass Init and it
      // gets unloaded, should we guess it can be reloaded?
      fCanLoadClassInfo = kTRUE;
   }
}

//______________________________________________________________________________
void TClass::ResetCaches()
{
   // To clean out all caches.

   R__ASSERT(!TestBit(kLoading) && "Resetting the caches does not make sense during loading!" );

   // Not owning lists, don't call Delete(), but unload
   if (fData)
      fData->Unload();
   if (fEnums)
      fEnums->Unload();
   if (fMethod)
      fMethod->Unload();

   delete fAllPubData; fAllPubData = 0;

   if (fBase)
      fBase->Delete();
   delete fBase; fBase = 0;

   if (fRealData)
      fRealData->Delete();
   delete fRealData;  fRealData=0;
}

//______________________________________________________________________________
void TClass::ResetMenuList()
{
   // Resets the menu list to it's standard value.

   if (fClassMenuList)
      fClassMenuList->Delete();
   else
      fClassMenuList = new TList();
   fClassMenuList->Add(new TClassMenuItem(TClassMenuItem::kPopupStandardList, this));
}

//______________________________________________________________________________
void TClass::ls(Option_t *options) const
{
   // The ls function lists the contents of a class on stdout. Ls output
   // is typically much less verbose then Dump().
   // If options contains 'streamerinfo', run ls on the list of streamerInfos
   // and the list of conversion streamerInfos.

   TNamed::ls(options);
   if (options==0 || options[0]==0) return;

   if (strstr(options,"streamerinfo")!=0) {
      GetStreamerInfos()->ls(options);

      if (fConversionStreamerInfo.load()) {
         std::map<std::string, TObjArray*>::iterator it;
         std::map<std::string, TObjArray*>::iterator end = (*fConversionStreamerInfo).end();
         for( it = (*fConversionStreamerInfo).begin(); it != end; ++it ) {
            it->second->ls(options);
         }
      }
   }
}

//______________________________________________________________________________
void TClass::MakeCustomMenuList()
{
   // Makes a customizable version of the popup menu list, i.e. makes a list
   // of TClassMenuItem objects of methods accessible by context menu.
   // The standard (and different) way consists in having just one element
   // in this list, corresponding to the whole standard list.
   // Once the customizable version is done, one can remove or add elements.

   R__LOCKGUARD(gInterpreterMutex);
   TClassMenuItem *menuItem;

   // Make sure fClassMenuList is initialized and empty.
   GetMenuList()->Delete();

   TList* methodList = new TList;
   GetMenuItems(methodList);

   TMethod *method;
   TMethodArg *methodArg;
   TClass  *classPtr = 0;
   TIter next(methodList);

   while ((method = (TMethod*) next())) {
      // if go to a mother class method, add separator
      if (classPtr != method->GetClass()) {
         menuItem = new TClassMenuItem(TClassMenuItem::kPopupSeparator, this);
         fClassMenuList->AddLast(menuItem);
         classPtr = method->GetClass();
      }
      // Build the signature of the method
      TString sig;
      TList* margsList = method->GetListOfMethodArgs();
      TIter nextarg(margsList);
      while ((methodArg = (TMethodArg*)nextarg())) {
         sig = sig+","+methodArg->GetFullTypeName();
      }
      if (sig.Length()!=0) sig.Remove(0,1);  // remove first comma
      menuItem = new TClassMenuItem(TClassMenuItem::kPopupUserFunction, this,
                                    method->GetName(), method->GetName(),0,
                                    sig.Data(),-1,TClassMenuItem::kIsSelf);
      if (method->IsMenuItem() == kMenuToggle) menuItem->SetToggle();
      fClassMenuList->Add(menuItem);
   }
   delete methodList;
}

//______________________________________________________________________________
void TClass::Move(void *arenaFrom, void *arenaTo) const
{
   // Register the fact that an object was moved from the memory location
   // 'arenaFrom' to the memory location 'arenaTo'.

   // If/when we have access to a copy constructor (or better to a move
   // constructor), this function should also perform the data move.
   // For now we just information the repository.

   if ((GetState() <= kEmulated) && !fCollectionProxy) {
      MoveAddressInRepository("TClass::Move",arenaFrom,arenaTo,this);
   }
}

//______________________________________________________________________________
TList *TClass::GetMenuList() const {
   // Return the list of menu items associated with the class.
   if (!fClassMenuList) {
      fClassMenuList = new TList();
      fClassMenuList->Add(new TClassMenuItem(TClassMenuItem::kPopupStandardList, const_cast<TClass*>(this)));
   }
   return fClassMenuList;
}

//______________________________________________________________________________
TListOfFunctions *TClass::GetMethodList()
{
   // Return (create an empty one if needed) the list of functions.
   // The major difference with GetListOfMethod is that this returns
   // the internal type of fMethod and thus can not be made public.
   // It also never 'loads' the content of the list.

   if (!fMethod) fMethod = new TListOfFunctions(this);
   return fMethod;
}


//______________________________________________________________________________
TMethod *TClass::GetMethodAny(const char *method)
{
   // Return pointer to method without looking at parameters.
   // Does not look in (possible) base classes.
   // Has the side effect of loading all the TMethod object in the list
   // of the class.

   if (!HasInterpreterInfo()) return 0;
   return (TMethod*) GetListOfMethods()->FindObject(method);
}

//______________________________________________________________________________
TMethod *TClass::GetMethodAllAny(const char *method)
{
   // Return pointer to method without looking at parameters.
   // Does look in all base classes.

   if (!HasInterpreterInfo()) return 0;

   TMethod* m = GetMethodAny(method);
   if (m) return m;

   TBaseClass *base;
   TIter       nextb(GetListOfBases());
   while ((base = (TBaseClass *) nextb())) {
      TClass *c = base->GetClassPointer();
      if (c) {
         m = c->GetMethodAllAny(method);
         if (m) return m;
      }
   }

   return 0;
}

//______________________________________________________________________________
TMethod *TClass::GetMethod(const char *method, const char *params,
                           Bool_t objectIsConst /* = kFALSE */)
{
   // Find the best method (if there is one) matching the parameters.
   // The params string must contain argument values, like "3189, \"aap\", 1.3".
   // The function invokes GetClassMethod to search for a possible method
   // in the class itself or in its base classes. Returns 0 in case method
   // is not found.

   if (fCanLoadClassInfo) LoadClassInfo();
   if (!fClassInfo) return 0;

   if (!gInterpreter)
      Fatal("GetMethod", "gInterpreter not initialized");

   TInterpreter::DeclId_t decl = gInterpreter->GetFunctionWithValues(fClassInfo,
                                                                     method, params,
                                                                     objectIsConst);

   if (!decl) return 0;

   // search recursively in this class or its base classes
   TMethod* f = FindClassOrBaseMethodWithId(decl);
   if (f) return f;

   Error("GetMethod",
         "\nDid not find matching TMethod <%s> with \"%s\" %sfor %s",
         method,params,objectIsConst ? "const " : "", GetName());
   return 0;
}


//______________________________________________________________________________
TMethod* TClass::FindClassOrBaseMethodWithId(DeclId_t declId) {
   // Find a method with decl id in this class or its bases.

   TFunction *f = GetMethodList()->Get(declId);
   if (f) return (TMethod*)f;

   TBaseClass *base;
   TIter       next(GetListOfBases());
   while ((base = (TBaseClass *) next())) {
      TClass *clBase = base->GetClassPointer();
      if (clBase) {
         f = clBase->FindClassOrBaseMethodWithId(declId);
         if (f) return (TMethod*)f;
      }
   }
   return 0;
}

//______________________________________________________________________________
TMethod *TClass::GetMethodWithPrototype(const char *method, const char *proto,
                                        Bool_t objectIsConst /* = kFALSE */,
                                        ROOT::EFunctionMatchMode mode /* = ROOT::kConversionMatch */)
{
   // Find the method with a given prototype. The proto string must be of the
   // form: "char*,int,double". Returns 0 in case method is not found.

   if (fCanLoadClassInfo) LoadClassInfo();
   if (!fClassInfo) return 0;

   if (!gInterpreter)
      Fatal("GetMethodWithPrototype", "gInterpreter not initialized");

   TInterpreter::DeclId_t decl = gInterpreter->GetFunctionWithPrototype(fClassInfo,
                                                                  method, proto,
                                                            objectIsConst, mode);

   if (!decl) return 0;
   TMethod* f = FindClassOrBaseMethodWithId(decl);
   if (f) return f;
   Error("GetMethodWithPrototype",
         "\nDid not find matching TMethod <%s> with \"%s\" %sfor %s",
         method,proto,objectIsConst ? "const " : "", GetName());
   return 0;
}

//______________________________________________________________________________
TMethod *TClass::GetClassMethod(Long_t faddr)
{
   // Look for a method in this class that has the interface function
   // address faddr.

   if (!HasInterpreterInfo()) return 0;

   TMethod *m;
   TIter    next(GetListOfMethods());
   while ((m = (TMethod *) next())) {
      if (faddr == (Long_t)m->InterfaceMethod())
         return m;
   }
   return 0;
}

//______________________________________________________________________________
TMethod *TClass::GetClassMethod(const char *name, const char* params,
                                Bool_t objectIsConst /* = kFALSE */)
{
   // Look for a method in this class that has the name and matches the parameters.
   // The params string must contain argument values, like "3189, \"aap\", 1.3".
   // Returns 0 in case method is not found.
   // See TClass::GetMethod to also search the base classes.

   if (fCanLoadClassInfo) LoadClassInfo();
   if (!fClassInfo) return 0;

   if (!gInterpreter)
      Fatal("GetClassMethod", "gInterpreter not initialized");

   TInterpreter::DeclId_t decl = gInterpreter->GetFunctionWithValues(fClassInfo,
                                                                     name, params,
                                                                     objectIsConst);

   if (!decl) return 0;

   TFunction *f = GetMethodList()->Get(decl);

   return (TMethod*)f; // Could be zero if the decl is actually in a base class.
}

//______________________________________________________________________________
TMethod *TClass::GetClassMethodWithPrototype(const char *name, const char* proto,
                                             Bool_t objectIsConst /* = kFALSE */,
                      ROOT::EFunctionMatchMode mode /* = ROOT::kConversionMatch */)
{
   // Find the method with a given prototype. The proto string must be of the
   // form: "char*,int,double". Returns 0 in case method is not found.
   // See TClass::GetMethodWithPrototype to also search the base classes.

   if (fCanLoadClassInfo) LoadClassInfo();
   if (!fClassInfo) return 0;

   if (!gInterpreter)
      Fatal("GetClassMethodWithPrototype", "gInterpreter not initialized");

   TInterpreter::DeclId_t decl = gInterpreter->GetFunctionWithPrototype(fClassInfo,
                                                                        name, proto,
                                                                        objectIsConst,
                                                                        mode);

   if (!decl) return 0;

   TFunction *f = GetMethodList()->Get(decl);

   return (TMethod*)f; // Could be zero if the decl is actually in a base class.
}

//______________________________________________________________________________
Int_t TClass::GetNdata()
{
   // Return the number of data members of this class
   // Note that in case the list of data members is not yet created, it will be done
   // by GetListOfDataMembers().

   if (!HasDataMemberInfo()) return 0;

   TList *lm = GetListOfDataMembers();
   if (lm)
      return lm->GetSize();
   else
      return 0;
}

//______________________________________________________________________________
Int_t TClass::GetNmethods()
{
   // Return the number of methods of this class
   // Note that in case the list of methods is not yet created, it will be done
   // by GetListOfMethods().
   // This will also load/populate the list of methods, to get 'just' the
   // number of currently loaded methods use:
   //    cl->GetListOfMethods(false)->GetSize();

   if (!HasInterpreterInfo()) return 0;

   TList *lm = GetListOfMethods();
   if (lm)
      return lm->GetSize();
   else
      return 0;
}

//______________________________________________________________________________
TVirtualStreamerInfo* TClass::GetStreamerInfo(Int_t version /* = 0 */) const
{
   // returns a pointer to the TVirtualStreamerInfo object for version
   // If the object does not exist, it is created
   //
   // Note: There are two special version numbers:
   //
   //       0: Use the class version from the currently loaded class library.
   //      -1: Assume no class library loaded (emulated class).
   //
   // Warning:  If we create a new streamer info, whether or not the build
   //           optimizes is controlled externally to us by a global variable!
   //           Don't call us unless you have set that variable properly
   //           with TStreamer::Optimize()!
   //

   TVirtualStreamerInfo *guess = fLastReadInfo;
   if (guess && guess->GetClassVersion() == version) {
      // If the StreamerInfo is assigned to the fLastReadInfo, we are
      // guaranted it was built and compiled.
      return guess;
   }

   R__LOCKGUARD(gInterpreterMutex);

   // Handle special version, 0 means currently loaded version.
   // Warning:  This may be -1 for an emulated class.
   // If version == -2, the user is requested the emulated streamerInfo
   // for an abstract base class eventhough we have a dictionary for it.
   if (version == 0) {
      version = fClassVersion;
   }
   if (!fStreamerInfo) {
      TMmallocDescTemp setreset;
      fStreamerInfo = new TObjArray(version + 10, -2);
   } else {
      Int_t ninfos = fStreamerInfo->GetSize();
      if ((version < -1) || (version >= ninfos)) {
         Error("GetStreamerInfo", "class: %s, attempting to access a wrong version: %d", GetName(), version);
         // FIXME: Shouldn't we go to -1 here, or better just abort?
         version = 0;
      }
   }
   TVirtualStreamerInfo* sinfo = (TVirtualStreamerInfo*) fStreamerInfo->At(version);
   if (!sinfo && (version != fClassVersion)) {
      // When the requested version does not exist we return
      // the TVirtualStreamerInfo for the currently loaded class version.
      // FIXME: This arguably makes no sense, we should warn and return nothing instead.
      // Note: This is done for STL collections
      // Note: fClassVersion could be -1 here (for an emulated class).
      // This is also the code path take for unversioned classes.
      sinfo = (TVirtualStreamerInfo*) fStreamerInfo->At(fClassVersion);
   }
   if (!sinfo) {
      // We just were not able to find a streamer info, we have to make a new one.
      TMmallocDescTemp setreset;
      sinfo = TVirtualStreamerInfo::Factory()->NewInfo(const_cast<TClass*>(this));
      fStreamerInfo->AddAtAndExpand(sinfo, fClassVersion);
      if (gDebug > 0) {
         printf("Creating StreamerInfo for class: %s, version: %d\n", GetName(), fClassVersion);
      }
      if (HasDataMemberInfo() || fCollectionProxy) {
         // If we do not have a StreamerInfo for this version and we do not
         // have dictionary information nor a proxy, there is nothing to build!
         //
         sinfo->Build();
      }
   } else {
      if (!sinfo->IsCompiled()) {
         // Streamer info has not been compiled, but exists.
         // Therefore it was read in from a file and we have to do schema evolution?
         // Or it didn't have a dictionary before, but does now?
         sinfo->BuildOld();
      }
   }
   // Cache the current info if we now have it.
   if (version == fClassVersion) {
      fCurrentInfo = sinfo;
   }
   // If the compilation succeeded, remember this StreamerInfo.
   if (sinfo->IsCompiled()) fLastReadInfo = sinfo;
   return sinfo;
}

//______________________________________________________________________________
TVirtualStreamerInfo* TClass::GetStreamerInfoAbstractEmulated(Int_t version /* = 0 */) const
{
   // For the case where the requestor class is emulated and this class is abstract,
   // returns a pointer to the TVirtualStreamerInfo object for version with an emulated
   // representation whether or not the class is loaded.
   //
   // If the object does not exist, it is created
   //
   // Note: There are two special version numbers:
   //
   //       0: Use the class version from the currently loaded class library.
   //      -1: Assume no class library loaded (emulated class).
   //
   // Warning:  If we create a new streamer info, whether or not the build
   //           optimizes is controlled externally to us by a global variable!
   //           Don't call us unless you have set that variable properly
   //           with TStreamer::Optimize()!
   //

   R__LOCKGUARD(gInterpreterMutex);

   TString newname( GetName() );
   newname += "@@emulated";

   TClass *emulated = TClass::GetClass(newname);

   TVirtualStreamerInfo* sinfo = 0;

   if (emulated) {
      sinfo = emulated->GetStreamerInfo(version);
   }
   if (!sinfo) {
      // The emulated version of the streamerInfo is explicitly requested and has
      // not been built yet.

      sinfo = (TVirtualStreamerInfo*) fStreamerInfo->At(version);
      if (!sinfo && (version != fClassVersion)) {
         // When the requested version does not exist we return
         // the TVirtualStreamerInfo for the currently loaded class version.
         // FIXME: This arguably makes no sense, we should warn and return nothing instead.
         sinfo = (TVirtualStreamerInfo*) fStreamerInfo->At(fClassVersion);
      }
      if (!sinfo) {
         // Let's take the first available StreamerInfo as a start
         Int_t ninfos = fStreamerInfo->GetEntriesFast() - 1;
         for (Int_t i = -1; sinfo == 0 && i < ninfos; ++i) {
            sinfo =  (TVirtualStreamerInfo*) fStreamerInfo->UncheckedAt(i);
         }
      }
      if (sinfo) {
         sinfo = dynamic_cast<TVirtualStreamerInfo*>( sinfo->Clone() );
         if (sinfo) {
            sinfo->SetClass(0);
            sinfo->SetName( newname );
            sinfo->BuildCheck();
            sinfo->BuildOld();
            sinfo->GetClass()->AddRule(TString::Format("sourceClass=%s targetClass=%s",GetName(),newname.Data()));
         } else
            Error("GetStreamerInfoAbstractEmulated", "could not create TVirtualStreamerInfo");
      }
   }
   return sinfo;
}

//______________________________________________________________________________
TVirtualStreamerInfo* TClass::FindStreamerInfoAbstractEmulated(UInt_t checksum) const
{
   // For the case where the requestor class is emulated and this class is abstract,
   // returns a pointer to the TVirtualStreamerInfo object for version with an emulated
   // representation whether or not the class is loaded.
   //
   // If the object does not exist, it is created
   //
   // Warning:  If we create a new streamer info, whether or not the build
   //           optimizes is controlled externally to us by a global variable!
   //           Don't call us unless you have set that variable properly
   //           with TStreamer::Optimize()!
   //

   R__LOCKGUARD(gInterpreterMutex);

   TString newname( GetName() );
   newname += "@@emulated";

   TClass *emulated = TClass::GetClass(newname);

   TVirtualStreamerInfo* sinfo = 0;

   if (emulated) {
      sinfo = emulated->FindStreamerInfo(checksum);
   }
   if (!sinfo) {
      // The emulated version of the streamerInfo is explicitly requested and has
      // not been built yet.

      sinfo = (TVirtualStreamerInfo*) FindStreamerInfo(checksum);
      if (!sinfo && (checksum != fCheckSum)) {
         // When the requested version does not exist we return
         // the TVirtualStreamerInfo for the currently loaded class version.
         // FIXME: This arguably makes no sense, we should warn and return nothing instead.
         sinfo = (TVirtualStreamerInfo*) fStreamerInfo->At(fClassVersion);
      }
      if (!sinfo) {
         // Let's take the first available StreamerInfo as a start
         Int_t ninfos = fStreamerInfo->GetEntriesFast() - 1;
         for (Int_t i = -1; sinfo == 0 && i < ninfos; ++i) {
            sinfo =  (TVirtualStreamerInfo*) fStreamerInfo->UncheckedAt(i);
         }
      }
      if (sinfo) {
         sinfo = dynamic_cast<TVirtualStreamerInfo*>( sinfo->Clone() );
         if (sinfo) {
            sinfo->SetClass(0);
            sinfo->SetName( newname );
            sinfo->BuildCheck();
            sinfo->BuildOld();
            sinfo->GetClass()->AddRule(TString::Format("sourceClass=%s targetClass=%s",GetName(),newname.Data()));
         } else
            Error("GetStreamerInfoAbstractEmulated", "could not create TVirtualStreamerInfo");
      }
   }
   return sinfo;
}

//______________________________________________________________________________
void TClass::IgnoreTObjectStreamer(Bool_t doIgnore)
{
   //  When the class kIgnoreTObjectStreamer bit is set, the automatically
   //  generated Streamer will not call TObject::Streamer.
   //  This option saves the TObject space overhead on the file.
   //  However, the information (fBits, fUniqueID) of TObject is lost.
   //
   //  Note that to be effective for objects streamed object-wise this function
   //  must be called for the class deriving directly from TObject, eg, assuming
   //  that BigTrack derives from Track and Track derives from TObject, one must do:
   //     Track::Class()->IgnoreTObjectStreamer();
   //  and not:
   //     BigTrack::Class()->IgnoreTObjectStreamer();
   //  To be effective for object streamed member-wise or split in a TTree,
   //  this function must be called for the most derived class (i.e. BigTrack).

   // We need to tak the lock since we are test and then setting fBits
   // and TStreamerInfo::fBits (and the StreamerInfo state in general)
   // which can also be modified by another thread.
   R__LOCKGUARD2(gInterpreterMutex);

   if ( doIgnore &&  TestBit(kIgnoreTObjectStreamer)) return;
   if (!doIgnore && !TestBit(kIgnoreTObjectStreamer)) return;
   TVirtualStreamerInfo *sinfo = GetCurrentStreamerInfo();
   if (sinfo) {
      if (sinfo->IsCompiled()) {
         // -- Warn the user that what they are doing cannot work.
         // Note: The reason is that TVirtualStreamerInfo::Build() examines
         // the kIgnoreTObjectStreamer bit and sets the TStreamerElement
         // type for the TObject base class streamer element it creates
         // to -1 as a flag.  Later on the TStreamerInfo::Compile()
         // member function sees the flag and does not insert the base
         // class element into the compiled streamer info.  None of this
         // machinery works correctly if we are called after the streamer
         // info has already been built and compiled.
         Error("IgnoreTObjectStreamer","Must be called before the creation of StreamerInfo");
         return;
      }
   }
   if (doIgnore) SetBit  (kIgnoreTObjectStreamer);
   else          ResetBit(kIgnoreTObjectStreamer);
}

//______________________________________________________________________________
Bool_t TClass::InheritsFrom(const char *classname) const
{
   // Return kTRUE if this class inherits from a class with name "classname".
   // note that the function returns kTRUE in case classname is the class itself

   if (strcmp(GetName(), classname) == 0) return kTRUE;

   return InheritsFrom(TClass::GetClass(classname,kTRUE,kTRUE));
}

//______________________________________________________________________________
Bool_t TClass::InheritsFrom(const TClass *cl) const
{
   // Return kTRUE if this class inherits from class cl.
   // note that the function returns KTRUE in case cl is the class itself

   if (!cl) return kFALSE;
   if (cl == this) return kTRUE;

   if (!HasDataMemberInfo()) {
      TVirtualStreamerInfo *sinfo = ((TClass *)this)->GetCurrentStreamerInfo();
      if (sinfo==0) sinfo = GetStreamerInfo();
      TIter next(sinfo->GetElements());
      TStreamerElement *element;
      while ((element = (TStreamerElement*)next())) {
         if (element->IsA() == TStreamerBase::Class()) {
            TClass *clbase = element->GetClassPointer();
            if (!clbase) return kFALSE; //missing class
            if (clbase->InheritsFrom(cl)) return kTRUE;
         }
      }
      return kFALSE;
   }
   // cast const away (only for member fBase which can be set in GetListOfBases())
   if (((TClass *)this)->GetBaseClass(cl)) return kTRUE;
   return kFALSE;
}

//______________________________________________________________________________
void *TClass::DynamicCast(const TClass *cl, void *obj, Bool_t up)
{
   // Cast obj of this class type up to baseclass cl if up is true.
   // Cast obj of this class type down from baseclass cl if up is false.
   // If this class is not a baseclass of cl return 0, else the pointer
   // to the cl part of this (up) or to this (down).

   if (cl == this) return obj;

   if (!HasDataMemberInfo()) return 0;

   Int_t off;
   if ((off = GetBaseClassOffset(cl)) != -1) {
      if (up)
         return (void*)((Long_t)obj+off);
      else
         return (void*)((Long_t)obj-off);
   }
   return 0;
}

//______________________________________________________________________________
const void *TClass::DynamicCast(const TClass *cl, const void *obj, Bool_t up)
{
   // Cast obj of this class type up to baseclass cl if up is true.
   // Cast obj of this class type down from baseclass cl if up is false.
   // If this class is not a baseclass of cl return 0, else the pointer
   // to the cl part of this (up) or to this (down).

   return DynamicCast(cl,const_cast<void*>(obj),up);
}

//______________________________________________________________________________
void *TClass::New(ENewType defConstructor, Bool_t quiet) const
{
   // Return a pointer to a newly allocated object of this class.
   // The class must have a default constructor. For meaning of
   // defConstructor, see TClass::IsCallingNew().
   //
   // If quiet is true, do no issue a message via Error on case
   // of problems, just return 0.
   //
   // The constructor actually called here can be customized by
   // using the rootcint pragma:
   //    #pragma link C++ ioctortype UserClass;
   // For example, with this pragma and a class named MyClass,
   // this method will called the first of the following 3
   // constructors which exists and is public:
   //    MyClass(UserClass*);
   //    MyClass(TRootIOCtor*);
   //    MyClass(); // Or a constructor with all its arguments defaulted.
   //
   // When more than one pragma ioctortype is used, the first seen as priority
   // For example with:
   //    #pragma link C++ ioctortype UserClass1;
   //    #pragma link C++ ioctortype UserClass2;
   // We look in the following order:
   //    MyClass(UserClass1*);
   //    MyClass(UserClass2*);
   //    MyClass(TRootIOCtor*);
   //    MyClass(); // Or a constructor with all its arguments defaulted.
   //

   void* p = 0;

   if (fNew) {
      // We have the new operator wrapper function,
      // so there is a dictionary and it was generated
      // by rootcint, so there should be a default
      // constructor we can call through the wrapper.
      TClass__GetCallingNew() = defConstructor;
      p = fNew(0);
      TClass__GetCallingNew() = kRealNew;
      if (!p && !quiet) {
         //Error("New", "cannot create object of class %s version %d", GetName(), fClassVersion);
         Error("New", "cannot create object of class %s", GetName());
      }
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the
      // constructor wrapper, so the dictionary was
      // not generated by rootcint.  Let's try to
      // create the object by having the interpreter
      // call the new operator, hopefully the class
      // library is loaded and there will be a default
      // constructor we can call.
      // [This is very unlikely to work, but who knows!]
      TClass__GetCallingNew() = defConstructor;
      R__LOCKGUARD2(gInterpreterMutex);
      p = gCling->ClassInfo_New(GetClassInfo());
      TClass__GetCallingNew() = kRealNew;
      if (!p && !quiet) {
         //Error("New", "cannot create object of class %s version %d", GetName(), fClassVersion);
         Error("New", "cannot create object of class %s", GetName());
      }
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      TClass__GetCallingNew() = defConstructor;
      p = fCollectionProxy->New();
      TClass__GetCallingNew() = kRealNew;
      if (!p && !quiet) {
         //Error("New", "cannot create object of class %s version %d", GetName(), fClassVersion);
         Error("New", "cannot create object of class %s", GetName());
      }
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling a
      // constructor (basically we just make sure that the
      // pointer data members are null, unless they are marked
      // as preallocated with the "->" comment, in which case
      // we default-construct an object to point at).

      // Do not register any TObject's that we create
      // as a result of creating this object.
      // FIXME: Why do we do this?
      // FIXME: Partial Answer: Is this because we may never actually deregister them???

      Bool_t statsave = GetObjectStat();
      if(statsave) {
         SetObjectStat(kFALSE);
      }
      TVirtualStreamerInfo* sinfo = GetStreamerInfo();
      if (!sinfo && !quiet) {
         Error("New", "Cannot construct class '%s' version %d, no streamer info available!", GetName(), fClassVersion);
         return 0;
      }

      TClass__GetCallingNew() = defConstructor;
      p = sinfo->New();
      TClass__GetCallingNew() = kRealNew;

      // FIXME: Mistake?  See note above at the GetObjectStat() call.
      // Allow TObject's to be registered again.
      if(statsave) {
         SetObjectStat(statsave);
      }

      // Register the object for special handling in the destructor.
      if (p) {
         RegisterAddressInRepository("New",p,this);
      } else {
         Error("New", "Failed to construct class '%s' using streamer info", GetName());
      }
   } else {
      Fatal("New", "This cannot happen!");
   }

   return p;
}

//______________________________________________________________________________
void *TClass::New(void *arena, ENewType defConstructor) const
{
   // Return a pointer to a newly allocated object of this class.
   // The class must have a default constructor. For meaning of
   // defConstructor, see TClass::IsCallingNew().

   void* p = 0;

   if (fNew) {
      // We have the new operator wrapper function,
      // so there is a dictionary and it was generated
      // by rootcint, so there should be a default
      // constructor we can call through the wrapper.
      TClass__GetCallingNew() = defConstructor;
      p = fNew(arena);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("New with placement", "cannot create object of class %s version %d at address %p", GetName(), fClassVersion, arena);
      }
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the
      // constructor wrapper, so the dictionary was
      // not generated by rootcint.  Let's try to
      // create the object by having the interpreter
      // call the new operator, hopefully the class
      // library is loaded and there will be a default
      // constructor we can call.
      // [This is very unlikely to work, but who knows!]
      TClass__GetCallingNew() = defConstructor;
      R__LOCKGUARD2(gInterpreterMutex);
      p = gCling->ClassInfo_New(GetClassInfo(),arena);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("New with placement", "cannot create object of class %s version %d at address %p", GetName(), fClassVersion, arena);
      }
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      TClass__GetCallingNew() = defConstructor;
      p = fCollectionProxy->New(arena);
      TClass__GetCallingNew() = kRealNew;
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling a
      // constructor (basically we just make sure that the
      // pointer data members are null, unless they are marked
      // as preallocated with the "->" comment, in which case
      // we default-construct an object to point at).

      // ???BUG???  ???WHY???
      // Do not register any TObject's that we create
      // as a result of creating this object.
      Bool_t statsave = GetObjectStat();
      if(statsave) {
         SetObjectStat(kFALSE);
      }

      TVirtualStreamerInfo* sinfo = GetStreamerInfo();
      if (!sinfo) {
         Error("New with placement", "Cannot construct class '%s' version %d at address %p, no streamer info available!", GetName(), fClassVersion, arena);
         return 0;
      }

      TClass__GetCallingNew() = defConstructor;
      p = sinfo->New(arena);
      TClass__GetCallingNew() = kRealNew;

      // ???BUG???
      // Allow TObject's to be registered again.
      if(statsave) {
         SetObjectStat(statsave);
      }

      // Register the object for special handling in the destructor.
      if (p) {
         RegisterAddressInRepository("TClass::New with placement",p,this);
      }
   } else {
      Error("New with placement", "This cannot happen!");
   }

   return p;
}

//______________________________________________________________________________
void *TClass::NewArray(Long_t nElements, ENewType defConstructor) const
{
   // Return a pointer to a newly allocated array of objects
   // of this class.
   // The class must have a default constructor. For meaning of
   // defConstructor, see TClass::IsCallingNew().

   void* p = 0;

   if (fNewArray) {
      // We have the new operator wrapper function,
      // so there is a dictionary and it was generated
      // by rootcint, so there should be a default
      // constructor we can call through the wrapper.
      TClass__GetCallingNew() = defConstructor;
      p = fNewArray(nElements, 0);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("NewArray", "cannot create object of class %s version %d", GetName(), fClassVersion);
      }
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the
      // constructor wrapper, so the dictionary was
      // not generated by rootcint.  Let's try to
      // create the object by having the interpreter
      // call the new operator, hopefully the class
      // library is loaded and there will be a default
      // constructor we can call.
      // [This is very unlikely to work, but who knows!]
      TClass__GetCallingNew() = defConstructor;
      R__LOCKGUARD2(gInterpreterMutex);
      p = gCling->ClassInfo_New(GetClassInfo(),nElements);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("NewArray", "cannot create object of class %s version %d", GetName(), fClassVersion);
      }
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      TClass__GetCallingNew() = defConstructor;
      p = fCollectionProxy->NewArray(nElements);
      TClass__GetCallingNew() = kRealNew;
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling a
      // constructor (basically we just make sure that the
      // pointer data members are null, unless they are marked
      // as preallocated with the "->" comment, in which case
      // we default-construct an object to point at).

      // ???BUG???  ???WHY???
      // Do not register any TObject's that we create
      // as a result of creating this object.
      Bool_t statsave = GetObjectStat();
      if(statsave) {
         SetObjectStat(kFALSE);
      }

      TVirtualStreamerInfo* sinfo = GetStreamerInfo();
      if (!sinfo) {
         Error("NewArray", "Cannot construct class '%s' version %d, no streamer info available!", GetName(), fClassVersion);
         return 0;
      }

      TClass__GetCallingNew() = defConstructor;
      p = sinfo->NewArray(nElements);
      TClass__GetCallingNew() = kRealNew;

      // ???BUG???
      // Allow TObject's to be registered again.
      if(statsave) {
         SetObjectStat(statsave);
      }

      // Register the object for special handling in the destructor.
      if (p) {
         RegisterAddressInRepository("TClass::NewArray",p,this);
      }
   } else {
      Error("NewArray", "This cannot happen!");
   }

   return p;
}

//______________________________________________________________________________
void *TClass::NewArray(Long_t nElements, void *arena, ENewType defConstructor) const
{
   // Return a pointer to a newly allocated object of this class.
   // The class must have a default constructor. For meaning of
   // defConstructor, see TClass::IsCallingNew().

   void* p = 0;

   if (fNewArray) {
      // We have the new operator wrapper function,
      // so there is a dictionary and it was generated
      // by rootcint, so there should be a default
      // constructor we can call through the wrapper.
      TClass__GetCallingNew() = defConstructor;
      p = fNewArray(nElements, arena);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("NewArray with placement", "cannot create object of class %s version %d at address %p", GetName(), fClassVersion, arena);
      }
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the constructor wrapper,
      // so the dictionary was not generated by rootcint (it was made either
      // by cint or by some external mechanism).  Let's try to create the
      // object by having the interpreter call the new operator, either the
      // class library is loaded and there is a default constructor we can
      // call, or the class is interpreted and we will call the default
      // constructor that way, or no default constructor is available and
      // we fail.
      TClass__GetCallingNew() = defConstructor;
      R__LOCKGUARD2(gInterpreterMutex);
      p = gCling->ClassInfo_New(GetClassInfo(),nElements, arena);
      TClass__GetCallingNew() = kRealNew;
      if (!p) {
         Error("NewArray with placement", "cannot create object of class %s version %d at address %p", GetName(), fClassVersion, arena);
      }
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      TClass__GetCallingNew() = defConstructor;
      p = fCollectionProxy->NewArray(nElements, arena);
      TClass__GetCallingNew() = kRealNew;
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling a
      // constructor (basically we just make sure that the
      // pointer data members are null, unless they are marked
      // as preallocated with the "->" comment, in which case
      // we default-construct an object to point at).

      // ???BUG???  ???WHY???
      // Do not register any TObject's that we create
      // as a result of creating this object.
      Bool_t statsave = GetObjectStat();
      if(statsave) {
         SetObjectStat(kFALSE);
      }

      TVirtualStreamerInfo* sinfo = GetStreamerInfo();
      if (!sinfo) {
         Error("NewArray with placement", "Cannot construct class '%s' version %d at address %p, no streamer info available!", GetName(), fClassVersion, arena);
         return 0;
      }

      TClass__GetCallingNew() = defConstructor;
      p = sinfo->NewArray(nElements, arena);
      TClass__GetCallingNew() = kRealNew;

      // ???BUG???
      // Allow TObject's to be registered again.
      if(statsave) {
         SetObjectStat(statsave);
      }

      if (fStreamerType & kEmulatedStreamer) {
         // We always register emulated objects, we need to always
         // use the streamer info to destroy them.
      }

      // Register the object for special handling in the destructor.
      if (p) {
         RegisterAddressInRepository("TClass::NewArray with placement",p,this);
      }
   } else {
      Error("NewArray with placement", "This cannot happen!");
   }

   return p;
}

//______________________________________________________________________________
void TClass::Destructor(void *obj, Bool_t dtorOnly)
{
   // Explicitly call destructor for object.

   // Do nothing if passed a null pointer.
   if (obj == 0) return;

   void* p = obj;

   if (dtorOnly && fDestructor) {
      // We have the destructor wrapper, use it.
      fDestructor(p);
   } else if ((!dtorOnly) && fDelete) {
      // We have the delete wrapper, use it.
      fDelete(p);
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the
      // destruct/delete wrapper, so the dictionary was
      // not generated by rootcint (it could have been
      // created by cint or by some external mechanism).
      // Let's have the interpreter call the destructor,
      // either the code will be in a loaded library,
      // or it will be interpreted, otherwise we fail
      // because there is no destructor code at all.
      if (dtorOnly) {
         R__LOCKGUARD2(gInterpreterMutex);
         gCling->ClassInfo_Destruct(fClassInfo,p);
      } else {
         R__LOCKGUARD2(gInterpreterMutex);
         gCling->ClassInfo_Delete(fClassInfo,p);
      }
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      fCollectionProxy->Destructor(p, dtorOnly);
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling a
      // destructor.

      Bool_t inRepo = kTRUE;
      Bool_t verFound = kFALSE;

      // Was this object allocated through TClass?
      std::multiset<Version_t> knownVersions;
      R__LOCKGUARD2(gOVRMutex);

      {
         RepoCont_t::iterator iter = gObjectVersionRepository.find(p);
         if (iter == gObjectVersionRepository.end()) {
            // No, it wasn't, skip special version handling.
            //Error("Destructor2", "Attempt to delete unregistered object of class '%s' at address %p!", GetName(), p);
            inRepo = kFALSE;
         } else {
            //objVer = iter->second;
            for (; (iter != gObjectVersionRepository.end()) && (iter->first == p); ++iter) {
               Version_t ver = iter->second.fVersion;
               knownVersions.insert(ver);
               if (ver == fClassVersion && this == iter->second.fClass) {
                  verFound = kTRUE;
               }
            }
         }
      }

      if (!inRepo || verFound) {
         // The object was allocated using code for the same class version
         // as is loaded now.  We may proceed without worry.
         TVirtualStreamerInfo* si = GetStreamerInfo();
         if (si) {
            si->Destructor(p, dtorOnly);
         } else {
            Error("Destructor", "No streamer info available for class '%s' version %d at address %p, cannot destruct emulated object!", GetName(), fClassVersion, p);
            Error("Destructor", "length of fStreamerInfo is %d", fStreamerInfo->GetSize());
            Int_t i = fStreamerInfo->LowerBound();
            for (Int_t v = 0; v < fStreamerInfo->GetSize(); ++v, ++i) {
               Error("Destructor", "fStreamerInfo->At(%d): %p", i, fStreamerInfo->At(i));
               if (fStreamerInfo->At(i) != 0) {
                  Error("Destructor", "Doing Dump() ...");
                  ((TVirtualStreamerInfo*)fStreamerInfo->At(i))->Dump();
               }
            }
         }
      } else {
         // The loaded class version is not the same as the version of the code
         // which was used to allocate this object.  The best we can do is use
         // the TVirtualStreamerInfo to try to free up some of the allocated memory.
         Error("Destructor", "Loaded class %s version %d is not registered for addr %p", GetName(), fClassVersion, p);
#if 0
         TVirtualStreamerInfo* si = (TVirtualStreamerInfo*) fStreamerInfo->At(objVer);
         if (si) {
            si->Destructor(p, dtorOnly);
         } else {
            Error("Destructor2", "No streamer info available for class '%s' version %d, cannot destruct object at addr: %p", GetName(), objVer, p);
            Error("Destructor2", "length of fStreamerInfo is %d", fStreamerInfo->GetSize());
            Int_t i = fStreamerInfo->LowerBound();
            for (Int_t v = 0; v < fStreamerInfo->GetSize(); ++v, ++i) {
               Error("Destructor2", "fStreamerInfo->At(%d): %p", i, fStreamerInfo->At(i));
               if (fStreamerInfo->At(i) != 0) {
                  // Do some debugging output.
                  Error("Destructor2", "Doing Dump() ...");
                  ((TVirtualStreamerInfo*)fStreamerInfo->At(i))->Dump();
               }
            }
         }
#endif
      }

      if (inRepo && verFound && p) {
         UnregisterAddressInRepository("TClass::Destructor",p,this);
      }
   } else {
      Error("Destructor", "This cannot happen! (class %s)", GetName());
   }
}

//______________________________________________________________________________
void TClass::DeleteArray(void *ary, Bool_t dtorOnly)
{
   // Explicitly call operator delete[] for an array.

   // Do nothing if passed a null pointer.
   if (ary == 0) return;

   // Make a copy of the address.
   void* p = ary;

   if (fDeleteArray) {
      if (dtorOnly) {
         Error("DeleteArray", "Destructor only is not supported!");
      } else {
         // We have the array delete wrapper, use it.
         fDeleteArray(ary);
      }
   } else if (HasInterpreterInfo()) {
      // We have the dictionary but do not have the
      // array delete wrapper, so the dictionary was
      // not generated by rootcint.  Let's try to
      // delete the array by having the interpreter
      // call the array delete operator, hopefully
      // the class library is loaded and there will be
      // a destructor we can call.
      R__LOCKGUARD2(gInterpreterMutex);
      gCling->ClassInfo_DeleteArray(GetClassInfo(),ary, dtorOnly);
   } else if (!HasInterpreterInfo() && fCollectionProxy) {
      // There is no dictionary at all, so this is an emulated
      // class; however we do have the services of a collection proxy,
      // so this is an emulated STL class.
      fCollectionProxy->DeleteArray(ary, dtorOnly);
   } else if (!HasInterpreterInfo() && !fCollectionProxy) {
      // There is no dictionary at all and we do not have
      // the services of a collection proxy available, so
      // use the streamer info to approximate calling the
      // array destructor.

      Bool_t inRepo = kTRUE;
      Bool_t verFound = kFALSE;

      // Was this array object allocated through TClass?
      std::multiset<Version_t> knownVersions;
      {
         R__LOCKGUARD2(gOVRMutex);
         RepoCont_t::iterator iter = gObjectVersionRepository.find(p);
         if (iter == gObjectVersionRepository.end()) {
            // No, it wasn't, we cannot know what to do.
            //Error("DeleteArray", "Attempt to delete unregistered array object, element type '%s', at address %p!", GetName(), p);
            inRepo = kFALSE;
         } else {
            for (; (iter != gObjectVersionRepository.end()) && (iter->first == p); ++iter) {
               Version_t ver = iter->second.fVersion;
               knownVersions.insert(ver);
               if (ver == fClassVersion && this == iter->second.fClass ) {
                  verFound = kTRUE;
               }
            }
         }
      }

      if (!inRepo || verFound) {
         // The object was allocated using code for the same class version
         // as is loaded now.  We may proceed without worry.
         TVirtualStreamerInfo* si = GetStreamerInfo();
         if (si) {
            si->DeleteArray(ary, dtorOnly);
         } else {
            Error("DeleteArray", "No streamer info available for class '%s' version %d at address %p, cannot destruct object!", GetName(), fClassVersion, ary);
            Error("DeleteArray", "length of fStreamerInfo is %d", fStreamerInfo->GetSize());
            Int_t i = fStreamerInfo->LowerBound();
            for (Int_t v = 0; v < fStreamerInfo->GetSize(); ++v, ++i) {
               Error("DeleteArray", "fStreamerInfo->At(%d): %p", v, fStreamerInfo->At(i));
               if (fStreamerInfo->At(i)) {
                  Error("DeleteArray", "Doing Dump() ...");
                  ((TVirtualStreamerInfo*)fStreamerInfo->At(i))->Dump();
               }
            }
         }
      } else {
         // The loaded class version is not the same as the version of the code
         // which was used to allocate this array.  The best we can do is use
         // the TVirtualStreamerInfo to try to free up some of the allocated memory.
         Error("DeleteArray", "Loaded class version %d is not registered for addr %p", fClassVersion, p);



#if 0
         TVirtualStreamerInfo* si = (TVirtualStreamerInfo*) fStreamerInfo->At(objVer);
         if (si) {
            si->DeleteArray(ary, dtorOnly);
         } else {
            Error("DeleteArray", "No streamer info available for class '%s' version %d at address %p, cannot destruct object!", GetName(), objVer, ary);
            Error("DeleteArray", "length of fStreamerInfo is %d", fStreamerInfo->GetSize());
            Int_t i = fStreamerInfo->LowerBound();
            for (Int_t v = 0; v < fStreamerInfo->GetSize(); ++v, ++i) {
               Error("DeleteArray", "fStreamerInfo->At(%d): %p", v, fStreamerInfo->At(i));
               if (fStreamerInfo->At(i)) {
                  // Print some debugging info.
                  Error("DeleteArray", "Doing Dump() ...");
                  ((TVirtualStreamerInfo*)fStreamerInfo->At(i))->Dump();
               }
            }
         }
#endif


      }

      // Deregister the object for special handling in the destructor.
      if (inRepo && verFound && p) {
         UnregisterAddressInRepository("TClass::DeleteArray",p,this);
      }
   } else {
      Error("DeleteArray", "This cannot happen! (class '%s')", GetName());
   }
}

//______________________________________________________________________________
void TClass::SetCanSplit(Int_t splitmode)
{
   // Set the splitability of this class:
   //   -1: Use the default calculation
   //    0: Disallow splitting
   //    1: Always allow splitting.

   fCanSplit = splitmode;
}

//______________________________________________________________________________
void TClass::SetClassVersion(Version_t version)
{
   // Private function.  Set the class version for the 'class' represented by
   // this TClass object.  See the public interface:
   //    ROOT::ResetClassVersion
   // defined in TClassTable.cxx
   //
   // Note on class version numbers:
   //   If no class number has been specified, TClass::GetVersion will return -1
   //   The Class Version 0 request the whole object to be transient
   //   The Class Version 1, unless specified via ClassDef indicates that the
   //      I/O should use the TClass checksum to distinguish the layout of the class

   fClassVersion = version;
   fCurrentInfo = 0;
}

//______________________________________________________________________________
TVirtualStreamerInfo* TClass::DetermineCurrentStreamerInfo()
{
   // Determine and set pointer to current TVirtualStreamerInfo

   R__LOCKGUARD2(gInterpreterMutex);
   if(!fCurrentInfo.load()) {
     fCurrentInfo=(TVirtualStreamerInfo*)(fStreamerInfo->At(fClassVersion));
   }
   return fCurrentInfo;
}

//______________________________________________________________________________
void TClass::SetCurrentStreamerInfo(TVirtualStreamerInfo *info)
{
   // Set pointer to current TVirtualStreamerInfo

   fCurrentInfo = info;
}

//______________________________________________________________________________
Int_t TClass::Size() const
{
   // Return size of object of this class.

   if (fSizeof!=-1) return fSizeof;
   if (fCollectionProxy) return fCollectionProxy->Sizeof();
   if (HasInterpreterInfo()) return gCling->ClassInfo_Size(GetClassInfo());
   return GetStreamerInfo()->GetSize();
}

//______________________________________________________________________________
TClass *TClass::Load(TBuffer &b)
{
   // Load class description from I/O buffer and return class object.

   UInt_t maxsize = 256;
   char *s = new char[maxsize];

   Int_t pos = b.Length();

   b.ReadString(s, maxsize); // Reads at most maxsize - 1 characters, plus null at end.
   while (strlen(s) == (maxsize - 1)) {
      // The classname is too large, try again with a large buffer.
      b.SetBufferOffset(pos);
      maxsize = 2*maxsize;
      delete [] s;
      s = new char[maxsize];
      b.ReadString(s, maxsize); // Reads at most maxsize - 1 characters, plus null at end.
   }

   TClass *cl = TClass::GetClass(s, kTRUE);
   if (!cl)
      ::Error("TClass::Load", "dictionary of class %s not found", s);

   delete [] s;
   return cl;
}

//______________________________________________________________________________
TClass *TClass::LoadClass(const char *requestedname, Bool_t silent)
{
   // Helper function used by TClass::GetClass().
   // This function attempts to load the dictionary for 'classname'
   // either from the TClassTable or from the list of generator.
   // If silent is 'true', do not warn about missing dictionary for the class.
   // (typically used for class that are used only for transient members)
   //
   // The 'requestedname' is expected to be already normalized.

   // This function does not (and should not) attempt to check in the
   // list of loaded classes or in the typedef.

   R__LOCKGUARD(gInterpreterMutex);

   TClass *result = LoadClassDefault(requestedname, silent);

   if (result) return result;
   else return LoadClassCustom(requestedname,silent);
}

//______________________________________________________________________________
TClass *TClass::LoadClassDefault(const char *requestedname, Bool_t /* silent */)
{
   // Helper function used by TClass::GetClass().
   // This function attempts to load the dictionary for 'classname' from
   // the TClassTable or the autoloader.
   // If silent is 'true', do not warn about missing dictionary for the class.
   // (typically used for class that are used only for transient members)
   //
   // The 'requestedname' is expected to be already normalized.

   // This function does not (and should not) attempt to check in the
   // list of loaded classes or in the typedef.

   DictFuncPtr_t dict = TClassTable::GetDictNorm(requestedname);

   if (!dict) {
      if (gInterpreter->AutoLoad(requestedname,kTRUE)) {
         dict = TClassTable::GetDictNorm(requestedname);
      }
   }

   if (dict) {
      TClass *ncl = (dict)();
      if (ncl) ncl->PostLoadCheck();
      return ncl;
   }
   return 0;
}

//______________________________________________________________________________
TClass *TClass::LoadClassCustom(const char *requestedname, Bool_t silent)
{
   // Helper function used by TClass::GetClass().
   // This function attempts to load the dictionary for 'classname'
   // from the list of generator.
   // If silent is 'true', do not warn about missing dictionary for the class.
   // (typically used for class that are used only for transient members)
   //
   // The 'requestedname' is expected to be already normalized.

   // This function does not (and should not) attempt to check in the
   // list of loaded classes or in the typedef.

   TIter next(gROOT->GetListOfClassGenerators());
   TClassGenerator *gen;
   while ((gen = (TClassGenerator*) next())) {
      TClass *cl = gen->GetClass(requestedname, kTRUE, silent);
      if (cl) {
         cl->PostLoadCheck();
         return cl;
      }
   }
   return 0;
}

//______________________________________________________________________________
void TClass::LoadClassInfo() const
{
   // Try to load the classInfo (it may require parsing the header file
   // and/or loading data from the clang pcm).

   R__LOCKGUARD(gInterpreterMutex);

   // If another thread executed LoadClassInfo at about the same time
   // as this thread return early since the work was done.
   if (!fCanLoadClassInfo) return;

   gInterpreter->AutoParse(GetName());
   if (!fClassInfo) gInterpreter->SetClassInfo(const_cast<TClass*>(this));   // sets fClassInfo pointer
   if (!gInterpreter->IsAutoParsingSuspended()) {
      if (!fClassInfo) {
	 ::Error("TClass::LoadClassInfo",
		 "no interpreter information for class %s is available eventhough it has a TClass initialization routine.",
		 fName.Data());
      }
      fCanLoadClassInfo = kFALSE;
   }
}

//______________________________________________________________________________
void TClass::Store(TBuffer &b) const
{
   // Store class description on I/O buffer.

   b.WriteString(GetName());
}

//______________________________________________________________________________
TClass *ROOT::CreateClass(const char *cname, Version_t id,
                          const type_info &info, TVirtualIsAProxy *isa,
                          const char *dfil, const char *ifil,
                          Int_t dl, Int_t il)
{
   // Global function called by a class' static Dictionary() method
   // (see the ClassDef macro).

   // When called via TMapFile (e.g. Update()) make sure that the dictionary
   // gets allocated on the heap and not in the mapped file.
   TMmallocDescTemp setreset;
   return new TClass(cname, id, info, isa, dfil, ifil, dl, il);
}

//______________________________________________________________________________
TClass *ROOT::CreateClass(const char *cname, Version_t id,
                          const char *dfil, const char *ifil,
                          Int_t dl, Int_t il)
{
   // Global function called by a class' static Dictionary() method
   // (see the ClassDef macro).

   // When called via TMapFile (e.g. Update()) make sure that the dictionary
   // gets allocated on the heap and not in the mapped file.
   TMmallocDescTemp setreset;
   return new TClass(cname, id, dfil, ifil, dl, il);
}

//______________________________________________________________________________
TClass::ENewType TClass::IsCallingNew()
{
   // Static method returning the defConstructor flag passed to TClass::New().
   // New type is either:
   //   TClass::kRealNew  - when called via plain new
   //   TClass::kClassNew - when called via TClass::New()
   //   TClass::kDummyNew - when called via TClass::New() but object is a dummy,
   //                       in which case the object ctor might take short cuts

   return TClass__GetCallingNew();
}

//______________________________________________________________________________
Bool_t TClass::IsLoaded() const
{
   // Return true if the shared library of this class is currently in the a
   // process's memory.  Return false, after the shared library has been
   // unloaded or if this is an 'emulated' class created from a file's StreamerInfo.

   return fState == kHasTClassInit;
}

//______________________________________________________________________________
Bool_t  TClass::IsStartingWithTObject() const
{
   // Returns true if this class inherits from TObject and if the start of
   // the TObject parts is at the very beginning of the objects.
   // Concretly this means that the following code is proper for this class:
   //     ThisClass *ptr;
   //     void *void_ptr = (void)ptr;
   //     TObject *obj = (TObject*)void_ptr;
   // This code would be wrong if 'ThisClass' did not inherit 'first' from
   // TObject.

   if (fProperty==(-1)) Property();
   return TestBit(kStartWithTObject);
}

//______________________________________________________________________________
Bool_t  TClass::IsTObject() const
{
   // Return kTRUE is the class inherits from TObject.

   if (fProperty==(-1)) Property();
   return TestBit(kIsTObject);
}

//______________________________________________________________________________
Bool_t  TClass::IsForeign() const
{
   // Return kTRUE is the class is Foreign (the class does not have a Streamer method).

   if (fProperty==(-1)) Property();
   return TestBit(kIsForeign);
}

//______________________________________________________________________________
void TClass::PostLoadCheck()
{
   // Do the initialization that can only be done after the CINT dictionary has
   // been fully populated and can not be delayed efficiently.

   // In the case of a Foreign class (loaded class without a Streamer function)
   // we reset fClassVersion to be -1 so that the current TVirtualStreamerInfo will not
   // be confused with a previously loaded streamerInfo.

   if (IsLoaded() && HasInterpreterInfo() && fClassVersion==1 /*&& fStreamerInfo
       && fStreamerInfo->At(1)*/ && IsForeign() )
   {
      SetClassVersion(-1);
   }
   else if (IsLoaded() && HasDataMemberInfo() && fStreamerInfo && (!IsForeign()||fClassVersion>1) )
   {
      R__LOCKGUARD(gInterpreterMutex);

      TVirtualStreamerInfo *info = (TVirtualStreamerInfo*)(fStreamerInfo->At(fClassVersion));
      // Here we need to check whether this TVirtualStreamerInfo (which presumably has been
      // loaded from a file) is consistent with the definition in the library we just loaded.
      // BuildCheck is not appropriate here since it check a streamerinfo against the
      // 'current streamerinfo' which, at time point, would be the same as 'info'!
      if (info && GetListOfDataMembers() && !GetCollectionProxy()
          && (info->GetCheckSum()!=GetCheckSum() && !info->CompareContent(this,0,kFALSE,kFALSE, 0) && !(MatchLegacyCheckSum(info->GetCheckSum()))))
      {
         Bool_t warn = ! TestBit(kWarned);
         if (warn && info->GetOldVersion()<=2) {
            // Names of STL base classes was modified in vers==3. Allocators removed
            //
            TIter nextBC(GetListOfBases());
            TBaseClass *bc;
            while ((bc=(TBaseClass*)nextBC()))
            {if (TClassEdit::IsSTLCont(bc->GetName())) warn = kFALSE;}
         }

         if (warn) {
            if (info->GetOnFileClassVersion()==1 && fClassVersion>1) {
               Warning("PostLoadCheck","\n\
   The class %s transitioned from not having a specified class version\n\
   to having a specified class version (the current class version is %d).\n\
   However too many different non-versioned layouts of the class have\n\
   already been loaded so far.  To work around this problem you can\n\
   load fewer 'old' file in the same ROOT session or load the C++ library\n\
   describing the class %s before opening the files or increase the version\n\
   number of the class for example ClassDef(%s,%d).\n\
   Do not try to write objects with the current class definition,\n\
   the files might not be readable.\n",
                       GetName(), fClassVersion, GetName(), GetName(), fStreamerInfo->GetLast()+1);
            } else {
               Warning("PostLoadCheck","\n\
   The StreamerInfo version %d for the class %s which was read\n\
   from a file previously opened has the same version as the active class\n\
   but a different checksum. You should update the version to ClassDef(%s,%d).\n\
   Do not try to write objects with the current class definition,\n\
   the files will not be readable.\n"
                       , fClassVersion, GetName(), GetName(), fStreamerInfo->GetLast()+1);
            }
            info->CompareContent(this,0,kTRUE,kTRUE,0);
            SetBit(kWarned);
         }
      }
   }
}

//______________________________________________________________________________
Long_t TClass::Property() const
{
   // Set TObject::fBits and fStreamerType to cache information about the
   // class.  The bits are
   //    kIsTObject : the class inherits from TObject
   //    kStartWithTObject:  TObject is the left-most class in the inheritance tree
   //    kIsForeign : the class doe not have a Streamer method
   // The value of fStreamerType are
   //    kTObject : the class inherits from TObject
   //    kForeign : the class does not have a Streamer method
   //    kInstrumented: the class does have a Streamer method
   //    kExternal: the class has a free standing way of streaming itself
   //    kEmulatedStreamer: the class is missing its shared library.

   R__LOCKGUARD(gInterpreterMutex);

   if (fProperty!=(-1)) return fProperty;

   // Avoid asking about the class when it is still building
   if (TestBit(kLoading)) return fProperty;

   // When called via TMapFile (e.g. Update()) make sure that the dictionary
   // gets allocated on the heap and not in the mapped file.
   TMmallocDescTemp setreset;

   TClass *kl = const_cast<TClass*>(this);

   kl->fStreamerType = TClass::kDefault;
   kl->fStreamerImpl = &TClass::StreamerDefault;

   if (InheritsFrom(TObject::Class())) {
      kl->SetBit(kIsTObject);

      // Is it DIRECT inheritance from TObject?
      Int_t delta = kl->GetBaseClassOffsetRecurse(TObject::Class());
      if (delta==0) kl->SetBit(kStartWithTObject);

      kl->fStreamerType  = kTObject;
      kl->fStreamerImpl  = &TClass::StreamerTObject;
   }

   if (HasInterpreterInfo()) {

      // This code used to use ClassInfo_Has|IsValidMethod but since v6
      // they return true if the routine is defined in the class or any of
      // its parent.  We explicitly want to know whether the function is
      // defined locally.
      if (!const_cast<TClass*>(this)->GetClassMethodWithPrototype("Streamer","TBuffer&",kFALSE)) {

         kl->SetBit(kIsForeign);
         kl->fStreamerType  = kForeign;
         kl->fStreamerImpl  = &TClass::StreamerStreamerInfo;

      } else if ( kl->fStreamerType == TClass::kDefault ) {
         if (kl->fStreamerFunc) {
            kl->fStreamerType  = kInstrumented;
            kl->fStreamerImpl  = &TClass::StreamerInstrumented;
         } else {
            // We have an automatic streamer using the StreamerInfo .. no need to go through the
            // Streamer method function itself.
            kl->fStreamerType  = kInstrumented;
            kl->fStreamerImpl  = &TClass::StreamerStreamerInfo;
         }
      }

      if (fStreamer) {
         kl->fStreamerType  = kExternal;
         kl->fStreamerImpl  = &TClass::StreamerExternal;
      }
      //must set this last since other threads may read fProperty
      // and think all test bits have been properly set
      kl->fProperty = gCling->ClassInfo_Property(fClassInfo);
      kl->fClassProperty = gCling->ClassInfo_ClassProperty(GetClassInfo());

   } else {

      if (fStreamer) {
         kl->fStreamerType  = kExternal;
         kl->fStreamerImpl  = &TClass::StreamerExternal;
      }

      kl->fStreamerType |= kEmulatedStreamer;
      kl->SetStreamerImpl();
      return 0;
   }

   return fProperty;
}

//_____________________________________________________________________________
void TClass::SetStreamerImpl()
{
   // Internal routine to set fStreamerImpl based on the value of
   // fStreamerType.

   switch (fStreamerType) {
      case kTObject:  fStreamerImpl  = &TClass::StreamerTObject; break;
      case kForeign:  fStreamerImpl  = &TClass::StreamerStreamerInfo; break;
      case kExternal: fStreamerImpl  = &TClass::StreamerExternal; break;
      case kInstrumented:  {
         if (fStreamerFunc) fStreamerImpl  = &TClass::StreamerInstrumented;
         else               fStreamerImpl  = &TClass::StreamerStreamerInfo;
         break;
      }

      case kEmulatedStreamer:               // intentional fall through
      case kForeign|kEmulatedStreamer:      // intentional fall through
      case kInstrumented|kEmulatedStreamer: fStreamerImpl = &TClass::StreamerStreamerInfo; break;
      case kExternal|kEmulatedStreamer:     fStreamerImpl = &TClass::StreamerExternal; break;
      case kTObject|kEmulatedStreamer:      fStreamerImpl = &TClass::StreamerTObjectEmulated; break;
      case TClass::kDefault:                fStreamerImpl = &TClass::StreamerDefault; break;
      default:
         Error("SetStreamerImpl","Unexpected value of fStreamerType: %d",fStreamerType);
   }
}


//_____________________________________________________________________________
void TClass::SetCollectionProxy(const ROOT::TCollectionProxyInfo &info)
{
   // Create the collection proxy object (and the streamer object) from
   // using the information in the TCollectionProxyInfo.

   R__LOCKGUARD(gInterpreterMutex);

   delete fCollectionProxy;

   // We can not use GetStreamerInfo() instead of TVirtualStreamerInfo::Factory()
   // because GetStreamerInfo call TStreamerInfo::Build which need to have fCollectionProxy
   // set correctly.

   TVirtualCollectionProxy *p = TVirtualStreamerInfo::Factory()->GenExplicitProxy(info,this);
   fCollectionProxy = p;

   AdoptStreamer(TVirtualStreamerInfo::Factory()->GenExplicitClassStreamer(info,this));

   if (fCollectionProxy && !fSchemaRules) {
      // Numeric Collections have implicit conversions:
      GetSchemaRules(kTRUE);
   }
   fCanSplit = -1;
}

//______________________________________________________________________________
void TClass::SetContextMenuTitle(const char *title)
{
   // Change (i.e. set) the title of the TNamed.

   fContextMenuTitle = title;
}

//______________________________________________________________________________
void TClass::SetGlobalIsA(IsAGlobalFunc_t func)
{
   // This function installs a global IsA function for this class.
   // The global IsA function will be used if there is no local IsA function (fIsA)
   //
   // A global IsA function has the signature:
   //
   //    TClass *func( TClass *cl, const void *obj);
   //
   // 'cl' is a pointer to the  TClass object that corresponds to the
   // 'pointer type' used to retrieve the value 'obj'
   //
   //  For example with:
   //    TNamed * m = new TNamed("example","test");
   //    TObject* o = m
   // and
   //    the global IsA function would be called with TObject::Class() as
   //    the first parameter and the exact numerical value in the pointer
   //    'o'.
   //
   //  In other word, inside the global IsA function. it is safe to C-style
   //  cast the value of 'obj' into a pointer to the class described by 'cl'.

   fGlobalIsA = func;
}

//______________________________________________________________________________
void TClass::SetUnloaded()
{
   // Call this method to indicate that the shared library containing this
   // class's code has been removed (unloaded) from the process's memory

   if (TestBit(kUnloaded) && !TestBit(kUnloading)) {
      // Don't redo the work.
      return;
   }
   SetBit(kUnloading);

   //R__ASSERT(fState == kLoaded);
   if (fState != kLoaded) {
      Fatal("SetUnloaded","The TClass for %s is being unloaded when in state %d\n",
            GetName(),(int)fState);
   }

   // Make sure SetClassInfo, re-calculated the state.
   fState = kForwardDeclared;

   delete fIsA; fIsA = 0;
   // Disable the autoloader while calling SetClassInfo, to prevent
   // the library from being reloaded!
   {
      int autoload_old = gCling->SetClassAutoloading(0);
      TInterpreter::SuspendAutoParsing autoParseRaii(gCling);

      gInterpreter->SetClassInfo(this,kTRUE);

      gCling->SetClassAutoloading(autoload_old);
   }
   fDeclFileName = 0;
   fDeclFileLine = 0;
   fImplFileName = 0;
   fImplFileLine = 0;
   fTypeInfo     = 0;

   if (fMethod) {
      fMethod->Unload();
   }
   if (fData) {
      fData->Unload();
   }
   if (fEnums) {
      fEnums->Unload();
   }

   if (fState <= kForwardDeclared && fStreamerInfo->GetEntries() != 0) {
      fState = kEmulated;
   }

   ResetBit(kUnloading);
   SetBit(kUnloaded);
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::SetStreamerInfo(Int_t /*version*/, const char * /*info*/)
{
   // Info is a string describing the names and types of attributes
   // written by the class Streamer function.
   // If info is an empty string (when called by TObject::StreamerInfo)
   // the default Streamer info string is build. This corresponds to
   // the case of an automatically generated Streamer.
   // In case of user defined Streamer function, it is the user responsability
   // to implement a StreamerInfo function (override TObject::StreamerInfo).
   // The user must call IsA()->SetStreamerInfo(info) from this function.

   // info is specified, nothing to do, except that we should verify
   // that it contains a valid descriptor.

/*
   TDataMember *dm;
   Int_t nch = strlen(info);
   Bool_t update = kTRUE;
   if (nch != 0) {
      //decode strings like "TObject;TAttLine;fA;fB;Int_t i,j,k;"
      char *save, *temp, *blank, *colon, *comma;
      save = new char[10000];
      temp = save;
      strlcpy(temp,info,10000);
      //remove heading and trailing blanks
      while (*temp == ' ') temp++;
      while (save[nch-1] == ' ') {nch--; save[nch] = 0;}
      if (nch == 0) {delete [] save; return;}
      if (save[nch-1] != ';') {save[nch] = ';'; save[nch+1] = 0;}
      //remove blanks around , or ;
      while ((blank = strstr(temp,"; "))) strcpy(blank+1,blank+2);
      while ((blank = strstr(temp," ;"))) strcpy(blank,  blank+1);
      while ((blank = strstr(temp,", "))) strcpy(blank+1,blank+2);
      while ((blank = strstr(temp," ,"))) strcpy(blank,  blank+1);
      while ((blank = strstr(temp,"  "))) strcpy(blank,  blank+1);
      //loop on tokens separated by ;
      char *final = new char[1000];
      char token[100];
      while ((colon=strchr(temp,';'))) {
         *colon = 0;
         strlcpy(token,temp,100);
         blank = strchr(token,' ');
         if (blank) {
            *blank = 0;
            if (!gROOT->GetType(token)) {
               Error("SetStreamerInfo","Illegal type: %s in %s",token,info);
               return;
            }
            while (blank) {
               strlcat(final,token,1000);
               strlcat(final," ",1000);
               comma = strchr(blank+1,','); if (comma) *comma=0;
               strlcat(final,blank+1,1000);
               strlcat(final,";",1000);
               blank = comma;
            }

         } else {
            if (TClass::GetClass(token,update)) {
               //a class name
               strlcat(final,token,1000); strlcat(final,";",1000);
            } else {
               //a data member name
               dm = (TDataMember*)GetListOfDataMembers()->FindObject(token);
               if (dm) {
                  strlcat(final,dm->GetFullTypeName(),1000);
                  strlcat(final," ",1000);
                  strlcat(final,token,1000); strlcat(final,";",1000);
               } else {
                  Error("SetStreamerInfo","Illegal name: %s in %s",token,info);
                  return;
               }
            }
            update = kFALSE;
         }
         temp = colon+1;
         if (*temp == 0) break;
      }
 ////     fStreamerInfo = final;
      delete [] final;
      delete [] save;
      return;
   }

   //info is empty. Let's build the default Streamer descriptor

   char *temp = new char[10000];
   temp[0] = 0;
   char local[100];

   //add list of base classes
   TIter nextb(GetListOfBases());
   TBaseClass *base;
   while ((base = (TBaseClass*) nextb())) {
      snprintf(local,100,"%s;",base->GetName());
      strlcat(temp,local,10000);
   }

   //add list of data members and types
   TIter nextd(GetListOfDataMembers());
   while ((dm = (TDataMember *) nextd())) {
      if (dm->IsEnum()) continue;
      if (!dm->IsPersistent()) continue;
      Long_t property = dm->Property();
      if (property & kIsStatic) continue;
      TClass *acl = TClass::GetClass(dm->GetTypeName(),update);
      update = kFALSE;
      if (acl) {
         if (acl->GetClassVersion() == 0) continue;
      }

      // dm->GetArrayIndex() returns an empty string if it does not
      // applies
      const char * index = dm->GetArrayIndex();
      if (strlen(index)==0)
         snprintf(local,100,"%s %s;",dm->GetFullTypeName(),dm->GetName());
      else
         snprintf(local,100,"%s %s[%s];",dm->GetFullTypeName(),dm->GetName(),index);
      strlcat(temp,local,10000);
   }
   //fStreamerInfo = temp;
   delete [] temp;
*/
   return 0;
}

//______________________________________________________________________________
Bool_t TClass::MatchLegacyCheckSum(UInt_t checksum) const
{
   // Return true if the checksum passed as argument is one of the checksum
   // value produced by the older checksum calulcation algorithm.

   for(UInt_t i = 1; i < kLatestCheckSum; ++i) {
      if ( checksum == GetCheckSum( (ECheckSum) i ) ) return kTRUE;
   }
   return kFALSE;
}

//______________________________________________________________________________
UInt_t TClass::GetCheckSum(ECheckSum code) const
{
   // Call GetCheckSum with validity check.

   bool isvalid;
   return GetCheckSum(code,isvalid);
}

//______________________________________________________________________________
UInt_t TClass::GetCheckSum(Bool_t &isvalid) const
{
   // Return GetCheckSum(kCurrentCheckSum,isvalid);

   return GetCheckSum(kCurrentCheckSum,isvalid);
}

//______________________________________________________________________________
UInt_t TClass::GetCheckSum(ECheckSum code, Bool_t &isvalid) const
{
   // Compute and/or return the class check sum.
   //
   // isvalid is set to false, if the function is unable to calculate the
   // checksum.
   //
   // The class ckecksum is used by the automatic schema evolution algorithm
   // to uniquely identify a class version.
   // The check sum is built from the names/types of base classes and
   // data members.
   // Original algorithm from Victor Perevovchikov (perev@bnl.gov).
   //
   // The valid range of code is determined by ECheckSum.
   //
   // kNoEnum:  data members of type enum are not counted in the checksum
   // kNoRange: return the checksum of data members and base classes, not including the ranges and array size found in comments.
   // kWithTypeDef: use the sugared type name in the calculation.
   //
   // This is needed for backward compatibility.
   //
   // WARNING: this function must be kept in sync with TStreamerInfo::GetCheckSum.
   // They are both used to handle backward compatibility and should both return the same values.
   // TStreamerInfo uses the information in TStreamerElement while TClass uses the information
   // from TClass::GetListOfBases and TClass::GetListOfDataMembers.

   R__LOCKGUARD(gInterpreterMutex);

   isvalid = kTRUE;

   if (fCheckSum && code == kCurrentCheckSum) return fCheckSum;

   // kCurrentCheckSum (0) is the default parameter value and should be kept
   // for backward compatibility, too be able to use the inequality checks,
   // we need to set the code to the largest value.
   if (code == kCurrentCheckSum) code = kLatestCheckSum;

   UInt_t id = 0;

   int il;
   TString name = GetName();
   TString type;
   il = name.Length();
   for (int i=0; i<il; i++) id = id*3+name[i];

   TList *tlb = ((TClass*)this)->GetListOfBases();
   if (tlb && !GetCollectionProxy()) {   // Loop over bases if not a proxied collection

      TIter nextBase(tlb);

      TBaseClass *tbc=0;
      while((tbc=(TBaseClass*)nextBase())) {
         name = tbc->GetName();
         Bool_t isSTL = TClassEdit::IsSTLCont(name);
         if (isSTL)
            name = TClassEdit::ShortType( name, TClassEdit::kDropStlDefault );
         il = name.Length();
         for (int i=0; i<il; i++) id = id*3+name[i];
         if (code > kNoBaseCheckSum && !isSTL) {
            if (tbc->GetClassPointer() == 0) {
               Error("GetCheckSum","Calculating the checksum for (%s) requires the base class (%s) meta information to be available!",
                     GetName(),tbc->GetName());
               isvalid = kFALSE;
               return 0;
            } else
               id = id*3 + tbc->GetClassPointer()->GetCheckSum();
         }
      }/*EndBaseLoop*/
   }
   TList *tlm = ((TClass*)this)->GetListOfDataMembers();
   if (tlm) {   // Loop over members
      TIter nextMemb(tlm);
      TDataMember *tdm=0;
      Long_t prop = 0;
      while((tdm=(TDataMember*)nextMemb())) {
         if (!tdm->IsPersistent())        continue;
         //  combine properties
         prop = (tdm->Property());
         TDataType* tdt = tdm->GetDataType();
         if (tdt) prop |= tdt->Property();

         if ( prop&kIsStatic)             continue;
         name = tdm->GetName(); il = name.Length();
         if ( (code > kNoEnum) && code != kReflex && code != kReflexNoComment && prop&kIsEnum)
            id = id*3 + 1;

         int i;
         for (i=0; i<il; i++) id = id*3+name[i];

         if (code > kWithTypeDef || code == kReflexNoComment) {
            type = tdm->GetTrueTypeName();
            if (TClassEdit::IsSTLCont(type))
               type = TClassEdit::ShortType( type, TClassEdit::kDropStlDefault );
            if (code == kReflex || code == kReflexNoComment) {
               if (prop&kIsEnum) {
                  type = "int";
               } else {
                  type.ReplaceAll("ULong64_t","unsigned long long");
                  type.ReplaceAll("Long64_t","long long");
                  type.ReplaceAll("<signed char","<char");
                  type.ReplaceAll(",signed char",",char");
                  if (type=="signed char") type = "char";
               }
            }
         } else {
            type = tdm->GetFullTypeName();
            if (TClassEdit::IsSTLCont(type))
               type = TClassEdit::ShortType( type, TClassEdit::kDropStlDefault );
         }

         il = type.Length();
         for (i=0; i<il; i++) id = id*3+type[i];

         int dim = tdm->GetArrayDim();
         if (prop&kIsArray) {
            for (int ii=0;ii<dim;ii++) id = id*3+tdm->GetMaxIndex(ii);
         }
         if (code > kNoRange) {
            const char *left;
            if (code > TClass::kNoRangeCheck)
               left = TVirtualStreamerInfo::GetElementCounterStart(tdm->GetTitle());
            else
               left = strstr(tdm->GetTitle(),"[");
            if (left) {
               const char *right = strstr(left,"]");
               if (right) {
                  ++left;
                  while (left != right) {
                     id = id*3 + *left;
                     ++left;
                  }
               }
            }
         }
      }/*EndMembLoop*/
   }
   if (code==kLatestCheckSum) fCheckSum = id;
   return id;
}

//______________________________________________________________________________
void TClass::AdoptReferenceProxy(TVirtualRefProxy* proxy)
{
   // Adopt the Reference proxy pointer to indicate that this class
   // represents a reference.
   // When a new proxy is adopted, the old one is deleted.

   R__LOCKGUARD(gInterpreterMutex);

   if ( fRefProxy )  {
      fRefProxy->Release();
   }
   fRefProxy = proxy;
   if ( fRefProxy )  {
      fRefProxy->SetClass(this);
   }
   fCanSplit = -1;
}

//______________________________________________________________________________
void TClass::AdoptMemberStreamer(const char *name, TMemberStreamer *p)
{
   // Adopt the TMemberStreamer pointer to by p and use it to Stream non basic
   // member name.

   if (!fRealData) return;

   R__LOCKGUARD(gInterpreterMutex);

   TIter next(fRealData);
   TRealData *rd;
   while ((rd = (TRealData*)next())) {
      if (strcmp(rd->GetName(),name) == 0) {
         // If there is a TStreamerElement that took a pointer to the
         // streamer we should inform it!
         rd->AdoptStreamer(p);
         break;
      }
   }

//  NOTE: This alternative was proposed but not is not used for now,
//  One of the major difference with the code above is that the code below
//  did not require the RealData to have been built
//    if (!fData) return;
//    const char *n = name;
//    while (*n=='*') n++;
//    TString ts(n);
//    int i = ts.Index("[");
//    if (i>=0) ts.Remove(i,999);
//    TDataMember *dm = (TDataMember*)fData->FindObject(ts.Data());
//    if (!dm) {
//       Warning("SetStreamer","Can not find member %s::%s",GetName(),name);
//       return;
//    }
//    dm->SetStreamer(p);
   return;
}

//______________________________________________________________________________
void TClass::SetMemberStreamer(const char *name, MemberStreamerFunc_t p)
{
   // Install a new member streamer (p will be copied).

   AdoptMemberStreamer(name,new TMemberStreamer(p));
}

//______________________________________________________________________________
Int_t TClass::ReadBuffer(TBuffer &b, void *pointer, Int_t version, UInt_t start, UInt_t count)
{
   // Function called by the Streamer functions to deserialize information
   // from buffer b into object at p.
   // This function assumes that the class version and the byte count information
   // have been read.
   //   version  is the version number of the class
   //   start    is the starting position in the buffer b
   //   count    is the number of bytes for this object in the buffer

   return b.ReadClassBuffer(this,pointer,version,start,count);
}

//______________________________________________________________________________
Int_t TClass::ReadBuffer(TBuffer &b, void *pointer)
{
   // Function called by the Streamer functions to deserialize information
   // from buffer b into object at p.

   return b.ReadClassBuffer(this,pointer);
}

//______________________________________________________________________________
Int_t TClass::WriteBuffer(TBuffer &b, void *pointer, const char * /*info*/)
{
   // Function called by the Streamer functions to serialize object at p
   // to buffer b. The optional argument info may be specified to give an
   // alternative StreamerInfo instead of using the default StreamerInfo
   // automatically built from the class definition.
   // For more information, see class TVirtualStreamerInfo.

   b.WriteClassBuffer(this,pointer);
   return 0;
}

//______________________________________________________________________________
void TClass::StreamerExternal(void *object, TBuffer &b, const TClass *onfile_class) const
{
   //There is special streamer for the class

   //      case kExternal:
   //      case kExternal|kEmulatedStreamer:

   TClassStreamer *streamer = gThreadTsd ? GetStreamer() : fStreamer;
   streamer->Stream(b,object,onfile_class);
}

//______________________________________________________________________________
void TClass::StreamerTObject(void *object, TBuffer &b, const TClass * /* onfile_class */) const
{
   // Case of TObjects

   // case kTObject:

   if (!fIsOffsetStreamerSet) {
      CalculateStreamerOffset();
   }
   TObject *tobj = (TObject*)((Long_t)object + fOffsetStreamer);
   tobj->Streamer(b);
}

//______________________________________________________________________________
void TClass::StreamerTObjectInitialized(void *object, TBuffer &b, const TClass * /* onfile_class */) const
{
   // Case of TObjects when fIsOffsetStreamerSet is known to have been set.

   TObject *tobj = (TObject*)((Long_t)object + fOffsetStreamer);
   tobj->Streamer(b);
}

//______________________________________________________________________________
void TClass::StreamerTObjectEmulated(void *object, TBuffer &b, const TClass *onfile_class) const
{
   // Case of TObjects when we do not have the library defining the class.

   // case kTObject|kEmulatedStreamer :
   if (b.IsReading()) {
      b.ReadClassEmulated(this, object, onfile_class);
   } else {
      b.WriteClassBuffer(this, object);
   }
}

//______________________________________________________________________________
void TClass::StreamerInstrumented(void *object, TBuffer &b, const TClass * /* onfile_class */) const
{
   // Case of instrumented class with a library

   // case kInstrumented:
   fStreamerFunc(b,object);
}

//______________________________________________________________________________
void TClass::StreamerStreamerInfo(void *object, TBuffer &b, const TClass *onfile_class) const
{
   // Case of where we should directly use the StreamerInfo.
   //    case kForeign:
   //    case kForeign|kEmulatedStreamer:
   //    case kInstrumented|kEmulatedStreamer:
   //    case kEmulatedStreamer:

   if (b.IsReading()) {
      b.ReadClassBuffer(this, object, onfile_class);
      //ReadBuffer (b, object);
   } else {
      //WriteBuffer(b, object);
      b.WriteClassBuffer(this, object);
   }
}

//______________________________________________________________________________
void TClass::StreamerDefault(void *object, TBuffer &b, const TClass *onfile_class) const
{
   // Default streaming in cases where either we have no way to know what to do
   // or if Property() has not yet been called.

   if (fProperty==(-1)) {
      Property();
      if (fStreamerImpl == &TClass::StreamerDefault) {
         Fatal("StreamerDefault", "fStreamerImpl not properly initialized (%d)", fStreamerType);
      } else {
         (this->*fStreamerImpl)(object,b,onfile_class);
      }
   } else {
      Fatal("StreamerDefault", "fStreamerType not properly initialized (%d)", fStreamerType);
   }
}

//______________________________________________________________________________
void TClass::AdoptStreamer(TClassStreamer *str)
{
   // Adopt a TClassStreamer object.  Ownership is transfered to this TClass
   // object.

//    // This code can be used to quickly test the STL Emulation layer
//    Int_t k = TClassEdit::IsSTLCont(GetName());
//    if (k==1||k==-1) { delete str; return; }

   R__LOCKGUARD(gInterpreterMutex);

   if (fStreamer) delete fStreamer;
   if (str) {
      fStreamerType = kExternal | ( fStreamerType&kEmulatedStreamer );
      fStreamer = str;
      fStreamerImpl = &TClass::StreamerExternal;
   } else if (fStreamer) {
      // Case where there was a custom streamer and it is hereby removed,
      // we need to reset fStreamerType
      fStreamer = str;
      fStreamerType = TClass::kDefault;
      if (fProperty != -1) {
         fProperty = -1;
         Property();
      }
   }
}

//______________________________________________________________________________
void TClass::SetStreamerFunc(ClassStreamerFunc_t strm)
{
   // Set a wrapper/accessor function around this class custom streamer.

   if (fProperty != -1 &&
       ( (fStreamerFunc == 0 && strm != 0) || (fStreamerFunc != 0 && strm == 0) ) )
   {
      fStreamerFunc = strm;

      // Since initialization has already been done, make sure to tweak it
      // for the new state.
      if (HasInterpreterInfo() && fStreamerType != kTObject && !fStreamer) {
         fStreamerType  = kInstrumented;
         fStreamerImpl  = &TClass::StreamerInstrumented;
      }
   } else {
      fStreamerFunc = strm;
   }
   fCanSplit = -1;
}

//______________________________________________________________________________
void TClass::SetMerge(ROOT::MergeFunc_t newMerge)
{
   // Install a new wrapper around 'Merge'.

   fMerge = newMerge;
}

//______________________________________________________________________________
void TClass::SetResetAfterMerge(ROOT::ResetAfterMergeFunc_t newReset)
{
   // Install a new wrapper around 'ResetAfterMerge'.

   fResetAfterMerge = newReset;
}

//______________________________________________________________________________
void TClass::SetNew(ROOT::NewFunc_t newFunc)
{
   // Install a new wrapper around 'new'.

   fNew = newFunc;
}

//______________________________________________________________________________
void TClass::SetNewArray(ROOT::NewArrFunc_t newArrayFunc)
{
   // Install a new wrapper around 'new []'.

   fNewArray = newArrayFunc;
}

//______________________________________________________________________________
void TClass::SetDelete(ROOT::DelFunc_t deleteFunc)
{
   // Install a new wrapper around 'delete'.

   fDelete = deleteFunc;
}

//______________________________________________________________________________
void TClass::SetDeleteArray(ROOT::DelArrFunc_t deleteArrayFunc)
{
   // Install a new wrapper around 'delete []'.

   fDeleteArray = deleteArrayFunc;
}

//______________________________________________________________________________
void TClass::SetDestructor(ROOT::DesFunc_t destructorFunc)
{
   // Install a new wrapper around the destructor.

   fDestructor = destructorFunc;
}

//______________________________________________________________________________
void TClass::SetDirectoryAutoAdd(ROOT::DirAutoAdd_t autoAddFunc)
{
   // Install a new wrapper around the directory auto add function..
   // The function autoAddFunc has the signature void (*)(void *obj, TDirectory dir)
   // and should register 'obj' to the directory if dir is not null
   // and unregister 'obj' from its current directory if dir is null

   fDirAutoAdd = autoAddFunc;
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::FindStreamerInfo(UInt_t checksum) const
{
   // Find the TVirtualStreamerInfo in the StreamerInfos corresponding to checksum

   TVirtualStreamerInfo *guess = fLastReadInfo;
   if (guess && guess->GetCheckSum() == checksum) {
      return guess;
   } else {
      if (fCheckSum == checksum) return GetStreamerInfo();

      R__LOCKGUARD(gInterpreterMutex);
      Int_t ninfos = fStreamerInfo->GetEntriesFast()-1;
      for (Int_t i=-1;i<ninfos;++i) {
         // TClass::fStreamerInfos has a lower bound not equal to 0,
         // so we have to use At and should not use UncheckedAt
         TVirtualStreamerInfo *info = (TVirtualStreamerInfo*)fStreamerInfo->UncheckedAt(i);
         if (info && info->GetCheckSum() == checksum) {
            // R__ASSERT(i==info->GetClassVersion() || (i==-1&&info->GetClassVersion()==1));
            info->BuildOld();
            if (info->IsCompiled()) fLastReadInfo = info;
            return info;
         }
      }
      return 0;
   }
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::FindStreamerInfo(TObjArray* arr, UInt_t checksum) const
{
   // Find the TVirtualStreamerInfo in the StreamerInfos corresponding to checksum
   R__LOCKGUARD(gInterpreterMutex);
   Int_t ninfos = arr->GetEntriesFast()-1;
   for (Int_t i=-1;i<ninfos;i++) {
      // TClass::fStreamerInfos has a lower bound not equal to 0,
      // so we have to use At and should not use UncheckedAt
      TVirtualStreamerInfo *info = (TVirtualStreamerInfo*)arr->UncheckedAt(i);
      if (!info) continue;
      if (info->GetCheckSum() == checksum) {
         R__ASSERT(i==info->GetClassVersion() || (i==-1&&info->GetClassVersion()==1));
         return info;
      }
   }
   return 0;
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::GetConversionStreamerInfo( const char* classname, Int_t version ) const
{
   // Return a Conversion StreamerInfo from the class 'classname' for version number 'version' to this class, if any.

   TClass *cl = TClass::GetClass( classname );
   if( !cl )
      return 0;
   return GetConversionStreamerInfo( cl, version );
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::GetConversionStreamerInfo( const TClass* cl, Int_t version ) const
{
   // Return a Conversion StreamerInfo from the class represened by cl for version number 'version' to this class, if any.

   //----------------------------------------------------------------------------
   // Check if the classname was specified correctly
   //----------------------------------------------------------------------------
   if( !cl )
      return 0;

   if( cl == this )
      return GetStreamerInfo( version );

   //----------------------------------------------------------------------------
   // Check if we already have it
   //----------------------------------------------------------------------------
   TObjArray* arr = 0;
   if (fConversionStreamerInfo.load()) {
      std::map<std::string, TObjArray*>::iterator it;
      R__LOCKGUARD(gInterpreterMutex);

      it = (*fConversionStreamerInfo).find( cl->GetName() );

      if( it != (*fConversionStreamerInfo).end() ) {
         arr = it->second;
      }

      if( arr && version > -1 && version < arr->GetSize() && arr->At( version ) )
         return (TVirtualStreamerInfo*) arr->At( version );
   }

   R__LOCKGUARD(gInterpreterMutex);

   //----------------------------------------------------------------------------
   // We don't have the streamer info so find it in other class
   //----------------------------------------------------------------------------
   const TObjArray *clSI = cl->GetStreamerInfos();
   TVirtualStreamerInfo* info = 0;
   if( version >= -1 && version < clSI->GetSize() )
      info = (TVirtualStreamerInfo*)clSI->At( version );

   if (!info && cl->GetCollectionProxy()) {
      info = cl->GetStreamerInfo(); // instantiate the StreamerInfo for STL collections.
   }

   if( !info )
      return 0;

   //----------------------------------------------------------------------------
   // We have the right info so we need to clone it to create new object with
   // non artificial streamer elements and we should build it for current class
   //----------------------------------------------------------------------------
   info = (TVirtualStreamerInfo*)info->Clone();

   if( !info->BuildFor( this ) ) {
      delete info;
      return 0;
   }

   if (!info->IsCompiled()) {
      // Streamer info has not been compiled, but exists.
      // Therefore it was read in from a file and we have to do schema evolution?
      // Or it didn't have a dictionary before, but does now?
      info->BuildOld();
   }

   //----------------------------------------------------------------------------
   // Cache this treamer info
   //----------------------------------------------------------------------------
   if (!arr) {
      arr = new TObjArray(version+10, -1);
      if (!fConversionStreamerInfo.load()) {
         fConversionStreamerInfo = new std::map<std::string, TObjArray*>();
      }
      (*fConversionStreamerInfo)[cl->GetName()] = arr;
   }
   arr->AddAtAndExpand( info, info->GetClassVersion() );
   return info;
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::FindConversionStreamerInfo( const char* classname, UInt_t checksum ) const
{
   // Return a Conversion StreamerInfo from the class 'classname' for the layout represented by 'checksum' to this class, if any.

   TClass *cl = TClass::GetClass( classname );
   if( !cl )
      return 0;
   return FindConversionStreamerInfo( cl, checksum );
}

//______________________________________________________________________________
TVirtualStreamerInfo *TClass::FindConversionStreamerInfo( const TClass* cl, UInt_t checksum ) const
{
   // Return a Conversion StreamerInfo from the class represened by cl for the layout represented by 'checksum' to this class, if any.

   //---------------------------------------------------------------------------
   // Check if the classname was specified correctly
   //---------------------------------------------------------------------------
   if( !cl )
      return 0;

   if( cl == this )
      return FindStreamerInfo( checksum );

   //----------------------------------------------------------------------------
   // Check if we already have it
   //----------------------------------------------------------------------------
   TObjArray* arr = 0;
   TVirtualStreamerInfo* info = 0;
   if (fConversionStreamerInfo.load()) {
      std::map<std::string, TObjArray*>::iterator it;

      R__LOCKGUARD(gInterpreterMutex);

      it = (*fConversionStreamerInfo).find( cl->GetName() );

      if( it != (*fConversionStreamerInfo).end() ) {
         arr = it->second;
      }
      if (arr) {
         info = FindStreamerInfo( arr, checksum );
      }
   }

   if( info )
      return info;

   R__LOCKGUARD(gInterpreterMutex);

   //----------------------------------------------------------------------------
   // Get it from the foreign class
   //----------------------------------------------------------------------------
   info = cl->FindStreamerInfo( checksum );

   if( !info )
      return 0;

   //----------------------------------------------------------------------------
   // We have the right info so we need to clone it to create new object with
   // non artificial streamer elements and we should build it for current class
   //----------------------------------------------------------------------------
   info = (TVirtualStreamerInfo*)info->Clone();
   if( !info->BuildFor( this ) ) {
      delete info;
      return 0;
   }

   if (!info->IsCompiled()) {
      // Streamer info has not been compiled, but exists.
      // Therefore it was read in from a file and we have to do schema evolution?
      // Or it didn't have a dictionary before, but does now?
      info->BuildOld();
   }

   //----------------------------------------------------------------------------
   // Cache this treamer info
   //----------------------------------------------------------------------------
   if (!arr) {
      arr = new TObjArray(16, -2);
      if (!fConversionStreamerInfo.load()) {
         fConversionStreamerInfo = new std::map<std::string, TObjArray*>();
      }
      (*fConversionStreamerInfo)[cl->GetName()] = arr;
   }
   arr->AddAtAndExpand( info, info->GetClassVersion() );

   return info;
}

//______________________________________________________________________________
void TClass::RegisterStreamerInfo(TVirtualStreamerInfo *info)
{
   // Register the StreamerInfo in the given slot, change the State of the
   // TClass as appropriate.

   if (info) {
      R__LOCKGUARD(gInterpreterMutex);
      Int_t slot = info->GetClassVersion();
      if (fStreamerInfo->GetSize() > (slot-fStreamerInfo->LowerBound())
          && fStreamerInfo->At(slot) != 0
          && fStreamerInfo->At(slot) != info) {
         Error("RegisterStreamerInfo",
               "Register StreamerInfo for %s on non-empty slot (%d).",
               GetName(),slot);
      }
      fStreamerInfo->AddAtAndExpand(info, slot);
      if (fState <= kForwardDeclared) {
         fState = kEmulated;
      }
   }
}

//______________________________________________________________________________
void TClass::RemoveStreamerInfo(Int_t slot)
{
   // Remove and delete the StreamerInfo in the given slot.
   // Update the slot accordingly.

   if (fStreamerInfo->GetSize() >= slot) {
      R__LOCKGUARD(gInterpreterMutex);
      TVirtualStreamerInfo *info = (TVirtualStreamerInfo*)fStreamerInfo->At(slot);
      fStreamerInfo->RemoveAt(fClassVersion);
      delete info;
      if (fState == kEmulated && fStreamerInfo->GetEntries() == 0) {
         fState = kForwardDeclared;
      }
   }
}

//______________________________________________________________________________
Bool_t TClass::HasDefaultConstructor() const
{
   // Return true if we have access to a default constructor.


   if (fNew) return kTRUE;

   if (HasInterpreterInfo()) {
      R__LOCKGUARD(gInterpreterMutex);
      return gCling->ClassInfo_HasDefaultConstructor(GetClassInfo());
   }
   if (fCollectionProxy) {
      return kTRUE;
   }
   if (fCurrentInfo.load()) {
      // Emulated class, we know how to construct them via the TStreamerInfo
      return kTRUE;
   }
   return kFALSE;
}

//______________________________________________________________________________
ROOT::MergeFunc_t TClass::GetMerge() const
{
   // Return the wrapper around Merge.

   return fMerge;
}

//______________________________________________________________________________
ROOT::ResetAfterMergeFunc_t TClass::GetResetAfterMerge() const
{
   // Return the wrapper around Merge.

   return fResetAfterMerge;
}

//______________________________________________________________________________
ROOT::NewFunc_t TClass::GetNew() const
{
   // Return the wrapper around new ThisClass().

   return fNew;
}

//______________________________________________________________________________
ROOT::NewArrFunc_t TClass::GetNewArray() const
{
   // Return the wrapper around new ThisClass[].

   return fNewArray;
}

//______________________________________________________________________________
ROOT::DelFunc_t TClass::GetDelete() const
{
   // Return the wrapper around delete ThiObject.

   return fDelete;
}

//______________________________________________________________________________
ROOT::DelArrFunc_t TClass::GetDeleteArray() const
{
   // Return the wrapper around delete [] ThiObject.

   return fDeleteArray;
}

//______________________________________________________________________________
ROOT::DesFunc_t TClass::GetDestructor() const
{
   // Return the wrapper around the destructor

   return fDestructor;
}

//______________________________________________________________________________
ROOT::DirAutoAdd_t TClass::GetDirectoryAutoAdd() const
{
   // Return the wrapper around the directory auto add function.

   return fDirAutoAdd;
}
