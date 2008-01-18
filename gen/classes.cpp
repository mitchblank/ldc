#include <sstream>
#include "gen/llvm.h"

#include "mtype.h"
#include "aggregate.h"
#include "init.h"
#include "declaration.h"

#include "gen/irstate.h"
#include "gen/tollvm.h"
#include "gen/arrays.h"
#include "gen/logger.h"
#include "gen/classes.h"
#include "gen/structs.h"
#include "gen/functions.h"
#include "gen/runtime.h"
#include "gen/dvalue.h"

#include "ir/irstruct.h"

//////////////////////////////////////////////////////////////////////////////////////////

static void LLVM_AddBaseClassInterfaces(ClassDeclaration* target, BaseClasses* bcs)
{
    // add base class data members first
    for (int j=0; j<bcs->dim; j++)
    {
        BaseClass* bc = (BaseClass*)(bcs->data[j]);

        // resolve interfaces while we're at it
        if (bc->base->isInterfaceDeclaration())
        {
            Logger::println("adding interface '%s'", bc->base->toPrettyChars());
            IrInterface* iri = new IrInterface(bc, NULL);
            target->irStruct->interfaces.insert(std::make_pair(bc->base, iri));
            if (!target->isAbstract()) {
                // Fill in vtbl[]
                bc->fillVtbl(target, &bc->vtbl, 0);
            }
            DtoResolveClass(bc->base);
        }

        // base *classes* might add more interfaces?
        LLVM_AddBaseClassInterfaces(target, &bc->base->baseclasses);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

static void LLVM_AddBaseClassData(BaseClasses* bcs)
{
    // add base class data members first
    for (int j=0; j<bcs->dim; j++)
    {
        BaseClass* bc = (BaseClass*)(bcs->data[j]);

        // interfaces never add data fields
        if (bc->base->isInterfaceDeclaration())
            continue;

        // recursively add baseclass data
        LLVM_AddBaseClassData(&bc->base->baseclasses);

        Array* arr = &bc->base->fields;
        if (arr->dim == 0)
            continue;

        Logger::println("Adding base class members of %s", bc->base->toChars());
        LOG_SCOPE;

        for (int k=0; k < arr->dim; k++) {
            VarDeclaration* v = (VarDeclaration*)(arr->data[k]);
            v->toObjFile();
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoResolveClass(ClassDeclaration* cd)
{
    if (cd->llvmResolved) return;
    cd->llvmResolved = true;

    Logger::println("DtoResolveClass(%s): %s", cd->toPrettyChars(), cd->loc.toChars());
    LOG_SCOPE;

    // get the TypeClass
    assert(cd->type->ty == Tclass);
    TypeClass* ts = (TypeClass*)cd->type;

    // make sure the IrStruct is created
    IrStruct* irstruct = cd->irStruct;
    if (!irstruct) {
        irstruct = new IrStruct(ts);
        cd->irStruct = irstruct;
    }

    // resolve the base class
    if (cd->baseClass) {
        DtoResolveClass(cd->baseClass);
    }

    // resolve interface vtables
    /*if (cd->vtblInterfaces) {
        Logger::println("Vtbl interfaces for '%s'", cd->toPrettyChars());
        LOG_SCOPE;
        for (int i=0; i < cd->vtblInterfaces->dim; i++) {
            BaseClass *b = (BaseClass *)cd->vtblInterfaces->data[i];
            ClassDeclaration *id = b->base;
            Logger::println("Vtbl interface: '%s'", id->toPrettyChars());
            DtoResolveClass(id);
            // Fill in vtbl[]
            b->fillVtbl(cd, &b->vtbl, 1);
        }
    }*/

    gIR->structs.push_back(irstruct);
    gIR->classes.push_back(cd);

    // add vtable
    ts->llvmVtblType = new llvm::PATypeHolder(llvm::OpaqueType::get());
    const llvm::Type* vtabty = getPtrToType(ts->llvmVtblType->get());

    std::vector<const llvm::Type*> fieldtypes;
    fieldtypes.push_back(vtabty);

    // add monitor
    fieldtypes.push_back(getPtrToType(llvm::Type::Int8Ty));

    // add base class data fields first
    LLVM_AddBaseClassData(&cd->baseclasses);

    // then add own members
    for (int k=0; k < cd->members->dim; k++) {
        Dsymbol* dsym = (Dsymbol*)(cd->members->data[k]);
        dsym->toObjFile();
    }

    // resolve class data fields (possibly unions)
    Logger::println("doing class fields");

    if (irstruct->offsets.empty())
    {
        Logger::println("has no fields");
    }
    else
    {
        Logger::println("has fields");
        unsigned prevsize = (unsigned)-1;
        unsigned lastoffset = (unsigned)-1;
        const llvm::Type* fieldtype = NULL;
        VarDeclaration* fieldinit = NULL;
        size_t fieldpad = 0;
        int idx = 0;
        for (IrStruct::OffsetMap::iterator i=irstruct->offsets.begin(); i!=irstruct->offsets.end(); ++i) {
            // first iteration
            if (lastoffset == (unsigned)-1) {
                lastoffset = i->first;
                fieldtype = i->second.type;
                fieldinit = i->second.var;
                prevsize = getABITypeSize(fieldtype);
                i->second.var->irField->index = idx;
            }
            // colliding offset?
            else if (lastoffset == i->first) {
                size_t s = getABITypeSize(i->second.type);
                if (s > prevsize) {
                    fieldpad += s - prevsize;
                    prevsize = s;
                }
                cd->irStruct->hasUnions = true;
                i->second.var->irField->index = idx;
            }
            // intersecting offset?
            else if (i->first < (lastoffset + prevsize)) {
                size_t s = getABITypeSize(i->second.type);
                assert((i->first + s) <= (lastoffset + prevsize)); // this holds because all types are aligned to their size
                cd->irStruct->hasUnions = true;
                i->second.var->irField->index = idx;
                i->second.var->irField->indexOffset = (i->first - lastoffset) / s;
            }
            // fresh offset
            else {
                // commit the field
                fieldtypes.push_back(fieldtype);
                irstruct->defaultFields.push_back(fieldinit);
                if (fieldpad) {
                    fieldtypes.push_back(llvm::ArrayType::get(llvm::Type::Int8Ty, fieldpad));
                    irstruct->defaultFields.push_back(NULL);
                    idx++;
                }

                idx++;

                // start new
                lastoffset = i->first;
                fieldtype = i->second.type;
                fieldinit = i->second.var;
                prevsize = getABITypeSize(fieldtype);
                i->second.var->irField->index = idx;
                fieldpad = 0;
            }
        }
        fieldtypes.push_back(fieldtype);
        irstruct->defaultFields.push_back(fieldinit);
        if (fieldpad) {
            fieldtypes.push_back(llvm::ArrayType::get(llvm::Type::Int8Ty, fieldpad));
            irstruct->defaultFields.push_back(NULL);
        }
    }

    // populate interface map
    {
        Logger::println("Adding interfaces to '%s'", cd->toPrettyChars());
        LOG_SCOPE;
        LLVM_AddBaseClassInterfaces(cd, &cd->baseclasses);
        Logger::println("%d interfaces added", cd->irStruct->interfaces.size());
    }

    // add interface vtables at the end
    int interIdx = (int)fieldtypes.size();
    for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
    {
        ClassDeclaration* id = i->first;
        IrInterface* iri = i->second;

        // set vtbl type
        TypeClass* itc = (TypeClass*)id->type;
        const llvm::Type* ivtblTy = getPtrToType(itc->llvmVtblType->get());
        fieldtypes.push_back(ivtblTy);

        // fix the interface vtable type
        iri->vtblTy = isaStruct(itc->llvmVtblType->get());

        // set index
        iri->index = interIdx++;
    }
    Logger::println("%d interface vtables added", cd->irStruct->interfaces.size());

    // create type
    const llvm::StructType* structtype = llvm::StructType::get(fieldtypes);

    // refine abstract types for stuff like: class C {C next;}
    assert(irstruct->recty != 0);
    llvm::PATypeHolder& spa = irstruct->recty;
    llvm::cast<llvm::OpaqueType>(spa.get())->refineAbstractTypeTo(structtype);
    structtype = isaStruct(spa.get());

    // make it official
    if (!ts->llvmType)
        ts->llvmType = new llvm::PATypeHolder(structtype);
    else
        *ts->llvmType = structtype;
    spa = *ts->llvmType;

    // name the type
    gIR->module->addTypeName(cd->mangle(), ts->llvmType->get());

    // get interface info type
    const llvm::StructType* infoTy = DtoInterfaceInfoType();

    // create vtable type
    llvm::GlobalVariable* svtblVar = 0;
    std::vector<const llvm::Type*> sinits_ty;

    for (int k=0; k < cd->vtbl.dim; k++)
    {
        Dsymbol* dsym = (Dsymbol*)cd->vtbl.data[k];
        assert(dsym);
        //Logger::cout() << "vtblsym: " << dsym->toChars() << '\n';

        if (FuncDeclaration* fd = dsym->isFuncDeclaration()) {
            DtoResolveFunction(fd);
            //assert(fd->type->ty == Tfunction);
            //TypeFunction* tf = (TypeFunction*)fd->type;
            //const llvm::Type* fpty = getPtrToType(tf->llvmType->get());
            const llvm::FunctionType* vfty = DtoBaseFunctionType(fd);
            const llvm::Type* vfpty = getPtrToType(vfty);
            sinits_ty.push_back(vfpty);
        }
        else if (ClassDeclaration* cd2 = dsym->isClassDeclaration()) {
            Logger::println("*** ClassDeclaration in vtable: %s", cd2->toChars());
            const llvm::Type* cinfoty;
            if (cd->isInterfaceDeclaration()) {
                cinfoty = infoTy;
            }
            else if (cd != ClassDeclaration::classinfo) {
                cinfoty = ClassDeclaration::classinfo->type->llvmType->get();
            }
            else {
                // this is the ClassInfo class, the type is this type
                cinfoty = ts->llvmType->get();
            }
            const llvm::Type* cty = getPtrToType(cinfoty);
            sinits_ty.push_back(cty);
        }
        else
        assert(0);
    }

    assert(!sinits_ty.empty());
    const llvm::StructType* svtbl_ty = llvm::StructType::get(sinits_ty);

    std::string styname(cd->mangle());
    styname.append("__vtblType");
    gIR->module->addTypeName(styname, svtbl_ty);

    // refine for final vtable type
    llvm::cast<llvm::OpaqueType>(ts->llvmVtblType->get())->refineAbstractTypeTo(svtbl_ty);

    gIR->classes.pop_back();
    gIR->structs.pop_back();

    gIR->declareList.push_back(cd);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoDeclareClass(ClassDeclaration* cd)
{
    if (cd->llvmDeclared) return;
    cd->llvmDeclared = true;

    Logger::println("DtoDeclareClass(%s): %s", cd->toPrettyChars(), cd->loc.toChars());
    LOG_SCOPE;

    assert(cd->type->ty == Tclass);
    TypeClass* ts = (TypeClass*)cd->type;

    assert(cd->irStruct);
    IrStruct* irstruct = cd->irStruct;

    gIR->structs.push_back(irstruct);
    gIR->classes.push_back(cd);

    bool needs_definition = false;
    if (cd->getModule() == gIR->dmodule) {
        needs_definition = true;
    }

    llvm::GlobalValue::LinkageTypes _linkage = llvm::GlobalValue::ExternalLinkage;

    // interfaces have no static initializer
    // same goes for abstract classes
    if (!cd->isInterfaceDeclaration() && !cd->isAbstract()) {
        // vtable
        std::string varname("_D");
        varname.append(cd->mangle());
        varname.append("6__vtblZ");

        const llvm::StructType* svtbl_ty = isaStruct(ts->llvmVtblType->get());
        cd->irStruct->vtbl = new llvm::GlobalVariable(svtbl_ty, true, _linkage, 0, varname, gIR->module);
    }

    // get interface info type
    const llvm::StructType* infoTy = DtoInterfaceInfoType();

    // interface info array
    if (!cd->irStruct->interfaces.empty()) {
        // symbol name
        std::string nam = "_D";
        nam.append(cd->mangle());
        nam.append("16__interfaceInfosZ");
        // resolve array type
        const llvm::ArrayType* arrTy = llvm::ArrayType::get(infoTy, cd->irStruct->interfaces.size());
        // declare global
        irstruct->interfaceInfosTy = arrTy;
        irstruct->interfaceInfos = new llvm::GlobalVariable(arrTy, true, _linkage, NULL, nam, gIR->module);
    }

    // interfaces have no static initializer
    // same goes for abstract classes
    if (!cd->isInterfaceDeclaration() && !cd->isAbstract()) {
        // interface vtables
        unsigned idx = 0;
        for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
        {
            ClassDeclaration* id = i->first;
            IrInterface* iri = i->second;

            std::string nam("_D");
            nam.append(cd->mangle());
            nam.append("11__interface");
            nam.append(id->mangle());
            nam.append("6__vtblZ");

            assert(iri->vtblTy);
            iri->vtbl = new llvm::GlobalVariable(iri->vtblTy, true, _linkage, 0, nam, gIR->module);
            llvm::Constant* idxs[2] = {DtoConstUint(0), DtoConstUint(idx)};
            iri->info = llvm::ConstantExpr::getGetElementPtr(irstruct->interfaceInfos, idxs, 2);
            idx++;
        }

        // init
        std::string initname("_D");
        initname.append(cd->mangle());
        initname.append("6__initZ");

        llvm::GlobalVariable* initvar = new llvm::GlobalVariable(ts->llvmType->get(), true, _linkage, NULL, initname, gIR->module);
        cd->irStruct->init = initvar;
    }

    gIR->classes.pop_back();
    gIR->structs.pop_back();

    gIR->constInitList.push_back(cd);
    if (needs_definition)
        gIR->defineList.push_back(cd);

    // classinfo
    DtoDeclareClassInfo(cd);

    // typeinfo
    if (needs_definition)
        cd->type->getTypeInfo(NULL);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoConstInitClass(ClassDeclaration* cd)
{
    if (cd->llvmInitialized) return;
    cd->llvmInitialized = true;

    if (cd->isInterfaceDeclaration())
        return; // nothing to do

    Logger::println("DtoConstInitClass(%s): %s", cd->toPrettyChars(), cd->loc.toChars());
    LOG_SCOPE;

    IrStruct* irstruct = cd->irStruct;
    gIR->structs.push_back(irstruct);
    gIR->classes.push_back(cd);

    // get the struct (class) type
    assert(cd->type->ty == Tclass);
    TypeClass* ts = (TypeClass*)cd->type;
    const llvm::StructType* structtype = isaStruct(ts->llvmType->get());
    const llvm::StructType* vtbltype = isaStruct(ts->llvmVtblType->get());

    // make sure each offset knows its default initializer
    for (IrStruct::OffsetMap::iterator i=irstruct->offsets.begin(); i!=irstruct->offsets.end(); ++i)
    {
        IrStruct::Offset* so = &i->second;
        llvm::Constant* finit = DtoConstFieldInitializer(so->var->type, so->var->init);
        so->init = finit;
        so->var->irField->constInit = finit;
    }

    // fill out fieldtypes/inits
    std::vector<llvm::Constant*> fieldinits;

    // first field is always the vtable
    if (cd->isAbstract())
    {
        fieldinits.push_back(
            llvm::ConstantPointerNull::get(
                getPtrToType(
                    ts->llvmVtblType->get()
                )
            )
        );
    }
    else
    {
        assert(cd->irStruct->vtbl != 0);
        fieldinits.push_back(cd->irStruct->vtbl);
    }

    // then comes monitor
    fieldinits.push_back(llvm::ConstantPointerNull::get(getPtrToType(llvm::Type::Int8Ty)));

    // go through the field inits and build the default initializer
    size_t nfi = irstruct->defaultFields.size();
    for (size_t i=0; i<nfi; ++i) {
        llvm::Constant* c;
        if (irstruct->defaultFields[i]) {
            c = irstruct->defaultFields[i]->irField->constInit;
            assert(c);
        }
        else {
            const llvm::ArrayType* arrty = isaArray(structtype->getElementType(i+2));
            assert(arrty);
            std::vector<llvm::Constant*> vals(arrty->getNumElements(), llvm::ConstantInt::get(llvm::Type::Int8Ty, 0, false));
            c = llvm::ConstantArray::get(arrty, vals);
        }
        fieldinits.push_back(c);
    }

    // last comes interface vtables
    const llvm::StructType* infoTy = DtoInterfaceInfoType();
    for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
    {
        IrInterface* iri = i->second;
        iri->infoTy = infoTy;
        if (cd->isAbstract())
        {
            fieldinits.push_back(llvm::Constant::getNullValue(structtype->getElementType(iri->index)));
        }
        else
        {
            assert(iri->vtbl);
            fieldinits.push_back(iri->vtbl);
        }
    }

    // generate initializer
#if 0
    //Logger::cout() << cd->toPrettyChars() << " | " << *structtype << '\n';
    assert(fieldinits.size() == structtype->getNumElements());
    for(size_t i=0; i<structtype->getNumElements(); ++i) {
        Logger::cout() << "s#" << i << " = " << *structtype->getElementType(i) << '\n';
        Logger::cout() << "i#" << i << " = " << *fieldinits[i] << '\n';
        assert(fieldinits[i]->getType() == structtype->getElementType(i));
    }
#endif

#if 0
    for(size_t i=0; i<fieldinits.size(); ++i) {
        Logger::cout() << "i#" << i << " = " << *fieldinits[i]->getType() << '\n';
    }
#endif

    llvm::Constant* _init = llvm::ConstantStruct::get(structtype, fieldinits);
    assert(_init);
    cd->irStruct->constInit = _init;

    // abstract classes have no static vtable
    // neither do interfaces (on their own, the implementing class supplies the vtable)
    if (!cd->isInterfaceDeclaration() && !cd->isAbstract())
    {
        // generate vtable initializer
        std::vector<llvm::Constant*> sinits;

        for (int k=0; k < cd->vtbl.dim; k++)
        {
            Dsymbol* dsym = (Dsymbol*)cd->vtbl.data[k];
            assert(dsym);
            //Logger::cout() << "vtblsym: " << dsym->toChars() << '\n';

            if (FuncDeclaration* fd = dsym->isFuncDeclaration()) {
                DtoForceDeclareDsymbol(fd);
                assert(fd->irFunc->func);
                llvm::Constant* c = llvm::cast<llvm::Constant>(fd->irFunc->func);
                // cast if necessary (overridden method)
                if (c->getType() != vtbltype->getElementType(k))
                    c = llvm::ConstantExpr::getBitCast(c, vtbltype->getElementType(k));
                sinits.push_back(c);
            }
            else if (ClassDeclaration* cd2 = dsym->isClassDeclaration()) {
                assert(cd->irStruct->classInfo);
                llvm::Constant* c = cd->irStruct->classInfo;
                sinits.push_back(c);
            }
            else
            assert(0);
        }

        const llvm::StructType* svtbl_ty = isaStruct(ts->llvmVtblType->get());

#if 0
        for (size_t i=0; i< sinits.size(); ++i)
        {
            Logger::cout() << "field[" << i << "] = " << *svtbl_ty->getElementType(i) << '\n';
            Logger::cout() << "init [" << i << "] = " << *sinits[i]->getType() << '\n';
            assert(svtbl_ty->getElementType(i) == sinits[i]->getType());
        }
#endif

        llvm::Constant* cvtblInit = llvm::ConstantStruct::get(svtbl_ty, sinits);
        cd->irStruct->constVtbl = llvm::cast<llvm::ConstantStruct>(cvtblInit);

        // create interface vtable const initalizers
        for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
        {
            ClassDeclaration* id = i->first;
            assert(id->type->ty == Tclass);
            TypeClass* its = (TypeClass*)id->type;

            IrInterface* iri = i->second;
            BaseClass* b = iri->base;

            const llvm::StructType* ivtbl_ty = isaStruct(its->llvmVtblType->get());

            // generate interface info initializer
            std::vector<llvm::Constant*> infoInits;

            // classinfo
            assert(id->irStruct->classInfo);
            llvm::Constant* c = id->irStruct->classInfo;
            infoInits.push_back(c);

            // vtbl
            const llvm::Type* byteptrptrty = getPtrToType(getPtrToType(llvm::Type::Int8Ty));
            c = llvm::ConstantExpr::getBitCast(iri->vtbl, byteptrptrty);
            c = DtoConstSlice(DtoConstSize_t(b->vtbl.dim), c);
            infoInits.push_back(c);

            // offset
            // generate target independent offset with constGEP
            /*llvm::Value* cidx = DtoConstInt(iri->index);
            Logger::cout() << "offset to interface in class type: " << *cd->type->llvmType->get() << '\n';
            size_t ioff = gTargetData->getIndexedOffset(cd->type->llvmType->get(), &cidx, 1);
            infoInits.push_back(DtoConstUint(ioff));*/
            assert(iri->index >= 0);
            size_t ioff = gTargetData->getStructLayout(isaStruct(cd->type->llvmType->get()))->getElementOffset(iri->index);
            infoInits.push_back(DtoConstUint(ioff));

            // create interface info initializer constant
            iri->infoInit = llvm::cast<llvm::ConstantStruct>(llvm::ConstantStruct::get(iri->infoTy, infoInits));

            // generate vtable initializer
            std::vector<llvm::Constant*> iinits;

            // add interface info
            iinits.push_back(iri->info);

            for (int k=1; k < b->vtbl.dim; k++)
            {
                Logger::println("interface vtbl const init nr. %d", k);
                Dsymbol* dsym = (Dsymbol*)b->vtbl.data[k];
                assert(dsym);
                FuncDeclaration* fd = dsym->isFuncDeclaration();
                assert(fd);
                DtoForceDeclareDsymbol(fd);
                assert(fd->irFunc->func);
                llvm::Constant* c = llvm::cast<llvm::Constant>(fd->irFunc->func);

                // we have to bitcast, as the type created in ResolveClass expects a different this type
                c = llvm::ConstantExpr::getBitCast(c, iri->vtblTy->getContainedType(k));
                iinits.push_back(c);
            }

    #if 0
            for (size_t x=0; x< iinits.size(); ++x)
            {
                Logger::cout() << "field[" << x << "] = " << *ivtbl_ty->getElementType(x) << "\n\n";
                Logger::cout() << "init [" << x << "] = " << *iinits[x] << "\n\n";
                assert(ivtbl_ty->getElementType(x) == iinits[x]->getType());
            }
    #endif

            llvm::Constant* civtblInit = llvm::ConstantStruct::get(ivtbl_ty, iinits);
            iri->vtblInit = llvm::cast<llvm::ConstantStruct>(civtblInit);
        }
    }
    // we always generate interfaceinfos as best we can
    /*else
    {
        for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
        {
            ClassDeclaration* id = i->first;
            assert(id->type->ty == Tclass);
            TypeClass* its = (TypeClass*)id->type;

            IrInterface* iri = i->second;
            BaseClass* b = iri->base;

            // generate interface info initializer
            std::vector<llvm::Constant*> infoInits;

            // classinfo
            assert(id->irStruct->classInfo);
            llvm::Constant* c = id->irStruct->classInfo;
            infoInits.push_back(c);

            // vtbl
            const llvm::Type* byteptrptrty = getPtrToType(getPtrToType(llvm::Type::Int8Ty));
            c = DtoConstSlice(DtoConstSize_t(0), getNullPtr(byteptrptrty));
            infoInits.push_back(c);

            // offset
            infoInits.push_back(DtoConstInt(0));

            // create interface info initializer constant
            iri->infoInit = llvm::cast<llvm::ConstantStruct>(llvm::ConstantStruct::get(iri->infoTy, infoInits));
        }
    }*/

    gIR->classes.pop_back();
    gIR->structs.pop_back();
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoDefineClass(ClassDeclaration* cd)
{
    if (cd->llvmDefined) return;
    cd->llvmDefined = true;

    Logger::println("DtoDefineClass(%s): %s", cd->toPrettyChars(), cd->loc.toChars());
    LOG_SCOPE;

    // get the struct (class) type
    assert(cd->type->ty == Tclass);
    TypeClass* ts = (TypeClass*)cd->type;

    if (cd->getModule() == gIR->dmodule) {
        // interfaces don't have initializers
        // neither do abstract classes
        if (!cd->isInterfaceDeclaration() && !cd->isAbstract())
        {
            cd->irStruct->init->setInitializer(cd->irStruct->constInit);
            cd->irStruct->vtbl->setInitializer(cd->irStruct->constVtbl);

            // initialize interface vtables
            IrStruct* irstruct = cd->irStruct;
            std::vector<llvm::Constant*> infoInits;
            for (IrStruct::InterfaceIter i=irstruct->interfaces.begin(); i!=irstruct->interfaces.end(); ++i)
            {
                IrInterface* iri = i->second;
                iri->vtbl->setInitializer(iri->vtblInit);
                infoInits.push_back(iri->infoInit);
            }
            // initialize interface info array
            if (!infoInits.empty())
            {
                llvm::Constant* arrInit = llvm::ConstantArray::get(irstruct->interfaceInfosTy, infoInits);
                irstruct->interfaceInfos->setInitializer(arrInit);
            }
        }

        // generate classinfo
        DtoDefineClassInfo(cd);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoNewClass(TypeClass* tc, NewExp* newexp)
{
    // resolve type
    DtoForceDeclareDsymbol(tc->sym);

    // allocate
    llvm::Value* mem;
    if (newexp->onstack)
    {
        mem = new llvm::AllocaInst(DtoType(tc)->getContainedType(0), "newclass_alloca", gIR->topallocapoint());
    }
    else
    {
        llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_newclass");
        std::vector<llvm::Value*> args;
        args.push_back(tc->sym->irStruct->classInfo);
        mem = gIR->ir->CreateCall(fn, args.begin(), args.end(), "newclass_gc_alloc");
        mem = DtoBitCast(mem, DtoType(tc), "newclass_gc");
    }

    // init
    DtoInitClass(tc, mem);

    // init inner-class outer reference
    if (newexp->thisexp)
    {
        Logger::println("Resolving outer class");
        LOG_SCOPE;
        DValue* thisval = newexp->thisexp->toElem(gIR);
        size_t idx = 2;
        idx += tc->sym->irStruct->interfaces.size();
        llvm::Value* dst = thisval->getRVal();
        llvm::Value* src = DtoGEPi(mem,0,idx,"tmp");
        Logger::cout() << "dst: " << *dst << "\nsrc: " << *src << '\n';
        DtoStore(dst, src);
    }
    // set the context for nested classes
    else if (tc->sym->isNested())
    {
        Logger::println("Resolving nested context");
        LOG_SCOPE;
        size_t idx = 2;
        idx += tc->sym->irStruct->interfaces.size();
        llvm::Value* nest = gIR->func()->decl->irFunc->nestedVar;
        if (!nest)
            nest = gIR->func()->decl->irFunc->thisVar;
        assert(nest);
        llvm::Value* gep = DtoGEPi(mem,0,idx,"tmp");
        nest = DtoBitCast(nest, gep->getType()->getContainedType(0));
        DtoStore(nest, gep);
    }

    // call constructor
    if (newexp->arguments)
        return DtoCallClassCtor(tc, newexp->member, newexp->arguments, mem);

    // return default constructed class
    return new DImValue(tc, mem, false);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoInitClass(TypeClass* tc, llvm::Value* dst)
{
    size_t presz = 2*getABITypeSize(DtoSize_t());
    uint64_t n = getABITypeSize(tc->llvmType->get()) - presz;

    // set vtable field seperately, this might give better optimization
    assert(tc->sym->irStruct->vtbl);
    DtoStore(tc->sym->irStruct->vtbl, DtoGEPi(dst,0,0,"vtbl"));

    // monitor always defaults to zero
    llvm::Value* tmp = DtoGEPi(dst,0,1,"monitor");
    DtoStore(llvm::Constant::getNullValue(tmp->getType()->getContainedType(0)), tmp);

    // done?
    if (n == 0)
        return;

    // copy the rest from the static initializer
    assert(tc->sym->irStruct->init);
    assert(dst->getType() == tc->sym->irStruct->init->getType());

    const llvm::Type* arrty = getPtrToType(llvm::Type::Int8Ty);

    llvm::Value* dstarr = DtoGEPi(dst,0,2,"tmp");
    dstarr = DtoBitCast(dstarr, arrty);

    llvm::Value* srcarr = DtoGEPi(tc->sym->irStruct->init,0,2,"tmp");
    srcarr = DtoBitCast(srcarr, arrty);

    llvm::Function* fn = LLVM_DeclareMemCpy32();
    std::vector<llvm::Value*> llargs;
    llargs.resize(4);
    llargs[0] = dstarr;
    llargs[1] = srcarr;
    llargs[2] = llvm::ConstantInt::get(llvm::Type::Int32Ty, n, false);
    llargs[3] = llvm::ConstantInt::get(llvm::Type::Int32Ty, 0, false);

    new llvm::CallInst(fn, llargs.begin(), llargs.end(), "", gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoCallClassCtor(TypeClass* type, CtorDeclaration* ctor, Array* arguments, llvm::Value* mem)
{
    Logger::println("Calling constructor");
    LOG_SCOPE;

    assert(ctor);
    DtoForceDeclareDsymbol(ctor);
    llvm::Function* fn = ctor->irFunc->func;
    TypeFunction* tf = (TypeFunction*)DtoDType(ctor->type);

    std::vector<llvm::Value*> ctorargs;
    ctorargs.push_back(mem);
    for (size_t i=0; i<arguments->dim; ++i)
    {
        Expression* ex = (Expression*)arguments->data[i];
        Argument* fnarg = Argument::getNth(tf->parameters, i);
        DValue* argval = DtoArgument(fnarg, ex);
        llvm::Value* a = argval->getRVal();
        const llvm::Type* aty = fn->getFunctionType()->getParamType(i+1);
        if (a->getType() != aty)
            a = DtoBitCast(a, aty);
        ctorargs.push_back(a);
    }
    llvm::CallInst* call = new llvm::CallInst(fn, ctorargs.begin(), ctorargs.end(), "tmp", gIR->scopebb());
    call->setCallingConv(DtoCallingConv(LINKd));

    return new DImValue(type, call, false);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoCallClassDtors(TypeClass* tc, llvm::Value* instance)
{
    Array* arr = &tc->sym->dtors;
    for (size_t i=0; i<arr->dim; i++)
    {
        FuncDeclaration* fd = (FuncDeclaration*)arr->data[i];
        assert(fd->irFunc->func);
        new llvm::CallInst(fd->irFunc->func, instance, "", gIR->scopebb());
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoCastClass(DValue* val, Type* _to)
{
    Logger::println("DtoCastClass(%s, %s)", val->getType()->toChars(), _to->toChars());
    LOG_SCOPE;

    Type* to = DtoDType(_to);
    if (to->ty == Tpointer) {
        const llvm::Type* tolltype = DtoType(_to);
        llvm::Value* rval = DtoBitCast(val->getRVal(), tolltype);
        return new DImValue(_to, rval);
    }

    assert(to->ty == Tclass);
    TypeClass* tc = (TypeClass*)to;

    Type* from = DtoDType(val->getType());
    TypeClass* fc = (TypeClass*)from;

    if (tc->sym->isInterfaceDeclaration()) {
        Logger::println("to interface");
        if (fc->sym->isInterfaceDeclaration()) {
            Logger::println("from interface");
            return DtoDynamicCastInterface(val, _to);
        }
        else {
            Logger::println("from object");
            return DtoDynamicCastObject(val, _to);
        }
    }
    else {
        Logger::println("to object");
        int poffset;
        if (fc->sym->isInterfaceDeclaration()) {
            Logger::println("interface cast");
            return DtoCastInterfaceToObject(val, _to);
        }
        else if (!tc->sym->isInterfaceDeclaration() && tc->sym->isBaseOf(fc->sym,NULL)) {
            Logger::println("static down cast)");
            const llvm::Type* tolltype = DtoType(_to);
            llvm::Value* rval = DtoBitCast(val->getRVal(), tolltype);
            return new DImValue(_to, rval);
        }
        else {
            Logger::println("dynamic up cast");
            return DtoDynamicCastObject(val, _to);
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoDynamicCastObject(DValue* val, Type* _to)
{
    // call:
    // Object _d_dynamic_cast(Object o, ClassInfo c)

    DtoForceDeclareDsymbol(ClassDeclaration::object);
    DtoForceDeclareDsymbol(ClassDeclaration::classinfo);

    llvm::Function* func = LLVM_D_GetRuntimeFunction(gIR->module, "_d_dynamic_cast");
    const llvm::FunctionType* funcTy = func->getFunctionType();

    std::vector<llvm::Value*> args;

    // Object o
    llvm::Value* tmp = val->getRVal();
    tmp = DtoBitCast(tmp, funcTy->getParamType(0));
    args.push_back(tmp);
    assert(funcTy->getParamType(0) == tmp->getType());

    // ClassInfo c
    TypeClass* to = (TypeClass*)DtoDType(_to);
    DtoForceDeclareDsymbol(to->sym);
    assert(to->sym->irStruct->classInfo);
    tmp = to->sym->irStruct->classInfo;
    // unfortunately this is needed as the implementation of object differs somehow from the declaration
    // this could happen in user code as well :/
    tmp = DtoBitCast(tmp, funcTy->getParamType(1));
    args.push_back(tmp);
    assert(funcTy->getParamType(1) == tmp->getType());

    // call it
    llvm::Value* ret = gIR->ir->CreateCall(func, args.begin(), args.end(), "tmp");

    // cast return value
    ret = DtoBitCast(ret, DtoType(_to));

    return new DImValue(_to, ret);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoCastInterfaceToObject(DValue* val, Type* to)
{
    // call:
    // Object _d_toObject(void* p)

    llvm::Function* func = LLVM_D_GetRuntimeFunction(gIR->module, "_d_toObject");
    const llvm::FunctionType* funcTy = func->getFunctionType();

    // void* p
    llvm::Value* tmp = val->getRVal();
    tmp = DtoBitCast(tmp, funcTy->getParamType(0));

    // call it
    llvm::Value* ret = gIR->ir->CreateCall(func, tmp, "tmp");

    // cast return value
    if (to != NULL)
        ret = DtoBitCast(ret, DtoType(to));
    else
        to = ClassDeclaration::object->type;

    return new DImValue(to, ret);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoDynamicCastInterface(DValue* val, Type* _to)
{
    // call:
    // Object _d_interface_cast(void* p, ClassInfo c)

    DtoForceDeclareDsymbol(ClassDeclaration::object);
    DtoForceDeclareDsymbol(ClassDeclaration::classinfo);

    llvm::Function* func = LLVM_D_GetRuntimeFunction(gIR->module, "_d_interface_cast");
    const llvm::FunctionType* funcTy = func->getFunctionType();

    std::vector<llvm::Value*> args;

    // void* p
    llvm::Value* tmp = val->getRVal();
    tmp = DtoBitCast(tmp, funcTy->getParamType(0));
    args.push_back(tmp);

    // ClassInfo c
    TypeClass* to = (TypeClass*)DtoDType(_to);
    DtoForceDeclareDsymbol(to->sym);
    assert(to->sym->irStruct->classInfo);
    tmp = to->sym->irStruct->classInfo;
    // unfortunately this is needed as the implementation of object differs somehow from the declaration
    // this could happen in user code as well :/
    tmp = DtoBitCast(tmp, funcTy->getParamType(1));
    args.push_back(tmp);

    // call it
    llvm::Value* ret = gIR->ir->CreateCall(func, args.begin(), args.end(), "tmp");

    // cast return value
    ret = DtoBitCast(ret, DtoType(_to));

    return new DImValue(_to, ret);
}

//////////////////////////////////////////////////////////////////////////////////////////

static unsigned LLVM_ClassOffsetToIndex(ClassDeclaration* cd, unsigned os, unsigned& idx)
{
    // start at the bottom of the inheritance chain
    if (cd->baseClass != 0) {
        unsigned o = LLVM_ClassOffsetToIndex(cd->baseClass, os, idx);
        if (o != (unsigned)-1)
            return o;
    }

    // check this class
    unsigned i;
    for (i=0; i<cd->fields.dim; ++i) {
        VarDeclaration* vd = (VarDeclaration*)cd->fields.data[i];
        if (os == vd->offset)
            return i+idx;
    }
    idx += i;

    return (unsigned)-1;
}

//////////////////////////////////////////////////////////////////////////////////////////

void ClassDeclaration::offsetToIndex(Type* t, unsigned os, std::vector<unsigned>& result)
{
    unsigned idx = 0;
    unsigned r = LLVM_ClassOffsetToIndex(this, os, idx);
    assert(r != (unsigned)-1 && "Offset not found in any aggregate field");
    // vtable is 0, monitor is 1
    r += 2;
    // interface offset further
    r += vtblInterfaces->dim;
    // the final index was not pushed
    result.push_back(r); 
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::Value* DtoIndexClass(llvm::Value* ptr, ClassDeclaration* cd, Type* t, unsigned os, std::vector<unsigned>& idxs)
{
    Logger::println("checking for offset %u type %s:", os, t->toChars());
    LOG_SCOPE;

    if (idxs.empty())
        idxs.push_back(0);

    const llvm::Type* llt = getPtrToType(DtoType(t));
    const llvm::Type* st = DtoType(cd->type);
    if (ptr->getType() != st) {
        assert(cd->irStruct->hasUnions);
        ptr = gIR->ir->CreateBitCast(ptr, st, "tmp");
    }

    unsigned dataoffset = 2;

    IrStruct* irstruct = cd->irStruct;
    for (IrStruct::OffsetMap::iterator i=irstruct->offsets.begin(); i!=irstruct->offsets.end(); ++i) {
    //for (unsigned i=0; i<cd->fields.dim; ++i) {
        //VarDeclaration* vd = (VarDeclaration*)cd->fields.data[i];
        VarDeclaration* vd = i->second.var;
        assert(vd);
        Type* vdtype = DtoDType(vd->type);
        Logger::println("found %u type %s", vd->offset, vdtype->toChars());
        assert(vd->irField->index >= 0);
        if (os == vd->offset && vdtype == t) {
            idxs.push_back(vd->irField->index + dataoffset);
            Logger::cout() << "indexing: " << *ptr << '\n';
            ptr = DtoGEP(ptr, idxs, "tmp");
            if (ptr->getType() != llt)
                ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
            Logger::cout() << "indexing: " << *ptr << '\n';
            if (vd->irField->indexOffset)
                ptr = new llvm::GetElementPtrInst(ptr, DtoConstUint(vd->irField->indexOffset), "tmp", gIR->scopebb());
            Logger::cout() << "indexing: " << *ptr << '\n';
            return ptr;
        }
        else if (vdtype->ty == Tstruct && (vd->offset + vdtype->size()) > os) {
            TypeStruct* ts = (TypeStruct*)vdtype;
            StructDeclaration* ssd = ts->sym;
            idxs.push_back(vd->irField->index + dataoffset);
            if (vd->irField->indexOffset) {
                Logger::println("has union field offset");
                ptr = DtoGEP(ptr, idxs, "tmp");
                if (ptr->getType() != llt)
                    ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
                ptr = new llvm::GetElementPtrInst(ptr, DtoConstUint(vd->irField->indexOffset), "tmp", gIR->scopebb());
                std::vector<unsigned> tmp;
                return DtoIndexStruct(ptr, ssd, t, os-vd->offset, tmp);
            }
            else {
                const llvm::Type* sty = getPtrToType(DtoType(vd->type));
                if (ptr->getType() != sty) {
                    ptr = gIR->ir->CreateBitCast(ptr, sty, "tmp");
                    std::vector<unsigned> tmp;
                    return DtoIndexStruct(ptr, ssd, t, os-vd->offset, tmp);
                }
                else {
                    return DtoIndexStruct(ptr, ssd, t, os-vd->offset, idxs);
                }
            }
        }
    }

    assert(0);

    size_t llt_sz = getABITypeSize(llt->getContainedType(0));
    assert(os % llt_sz == 0);
    ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
    return new llvm::GetElementPtrInst(ptr, DtoConstUint(os / llt_sz), "tmp", gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::Value* DtoVirtualFunctionPointer(DValue* inst, FuncDeclaration* fdecl)
{
    assert(fdecl->isVirtual());//fdecl->isAbstract() || (!fdecl->isFinal() && fdecl->isVirtual()));
    assert(fdecl->vtblIndex > 0);
    assert(DtoDType(inst->getType())->ty == Tclass);

    llvm::Value* vthis = inst->getRVal();
    //Logger::cout() << "vthis: " << *vthis << '\n';

    llvm::Value* funcval;
    funcval = DtoGEPi(vthis, 0, 0, "tmp");
    funcval = DtoLoad(funcval);
    funcval = DtoGEPi(funcval, 0, fdecl->vtblIndex, fdecl->toPrettyChars());
    funcval = DtoLoad(funcval);

    //assert(funcval->getType() == DtoType(fdecl->type));
    //cc = DtoCallingConv(fdecl->linkage);

    return funcval;
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoDeclareClassInfo(ClassDeclaration* cd)
{
    if (cd->irStruct->classDeclared) return;
    cd->irStruct->classDeclared = true;

    Logger::println("DtoDeclareClassInfo(%s)", cd->toChars());
    LOG_SCOPE;

    ClassDeclaration* cinfo = ClassDeclaration::classinfo;
    DtoResolveClass(cinfo);

    std::string gname("_D");
    gname.append(cd->mangle());
    if (!cd->isInterfaceDeclaration())
        gname.append("7__ClassZ");
    else
        gname.append("11__InterfaceZ");

    const llvm::Type* st = cinfo->type->llvmType->get();

    cd->irStruct->classInfo = new llvm::GlobalVariable(st, true, llvm::GlobalValue::ExternalLinkage, NULL, gname, gIR->module);
}

static llvm::Constant* build_offti_entry(VarDeclaration* vd)
{
    std::vector<const llvm::Type*> types;
    std::vector<llvm::Constant*> inits;

    types.push_back(DtoSize_t());

    size_t offset = vd->offset; // TODO might not be the true offset
    // dmd only accounts for the vtable, not classinfo or monitor
    if (global.params.is64bit)
        offset += 8;
    else
        offset += 4;
    inits.push_back(DtoConstSize_t(offset));

    vd->type->getTypeInfo(NULL);
    assert(vd->type->vtinfo);
    DtoForceDeclareDsymbol(vd->type->vtinfo);
    llvm::Constant* c = isaConstant(vd->type->vtinfo->getIrValue());

    const llvm::Type* tiTy = getPtrToType(Type::typeinfo->type->llvmType->get());
    //Logger::cout() << "tiTy = " << *tiTy << '\n';

    types.push_back(tiTy);
    inits.push_back(llvm::ConstantExpr::getBitCast(c, tiTy));

    const llvm::StructType* sTy = llvm::StructType::get(types);
    return llvm::ConstantStruct::get(sTy, inits);
}

static llvm::Constant* build_offti_array(ClassDeclaration* cd, llvm::Constant* init)
{
    const llvm::StructType* initTy = isaStruct(init->getType());
    assert(initTy);

    std::vector<llvm::Constant*> arrayInits;
    for (ClassDeclaration *cd2 = cd; cd2; cd2 = cd2->baseClass)
    {
    if (cd2->members)
    {
        for (size_t i = 0; i < cd2->members->dim; i++)
        {
        Dsymbol *sm = (Dsymbol *)cd2->members->data[i];
        if (VarDeclaration* vd = sm->isVarDeclaration()) // is this enough?
        {
            llvm::Constant* c = build_offti_entry(vd);
            assert(c);
            arrayInits.push_back(c);
        }
        }
    }
    }

    size_t ninits = arrayInits.size();
    llvm::Constant* size = DtoConstSize_t(ninits);
    llvm::Constant* ptr;

    if (ninits > 0) {
        // OffsetTypeInfo type
        std::vector<const llvm::Type*> elemtypes;
        elemtypes.push_back(DtoSize_t());
        const llvm::Type* tiTy = getPtrToType(Type::typeinfo->type->llvmType->get());
        elemtypes.push_back(tiTy);
        const llvm::StructType* sTy = llvm::StructType::get(elemtypes);

        // array type
        const llvm::ArrayType* arrTy = llvm::ArrayType::get(sTy, ninits);
        llvm::Constant* arrInit = llvm::ConstantArray::get(arrTy, arrayInits);

        std::string name(cd->type->vtinfo->toChars());
        name.append("__OffsetTypeInfos");
        llvm::GlobalVariable* gvar = new llvm::GlobalVariable(arrTy,true,llvm::GlobalValue::InternalLinkage,arrInit,name,gIR->module);
        ptr = llvm::ConstantExpr::getBitCast(gvar, getPtrToType(sTy));
    }
    else {
        ptr = llvm::ConstantPointerNull::get(isaPointer(initTy->getElementType(1)));
    }

    return DtoConstSlice(size, ptr);
}

static llvm::Constant* build_class_dtor(ClassDeclaration* cd)
{
    // construct the function
    std::vector<const llvm::Type*> paramTypes;
    paramTypes.push_back(getPtrToType(cd->type->llvmType->get()));

    const llvm::FunctionType* fnTy = llvm::FunctionType::get(llvm::Type::VoidTy, paramTypes, false);

    if (cd->dtors.dim == 0) {
        return llvm::ConstantPointerNull::get(getPtrToType(llvm::Type::Int8Ty));
    }
    else if (cd->dtors.dim == 1) {
        DtorDeclaration *d = (DtorDeclaration *)cd->dtors.data[0];
        DtoForceDeclareDsymbol(d);
        assert(d->irFunc->func);
        return llvm::ConstantExpr::getBitCast(isaConstant(d->irFunc->func), getPtrToType(llvm::Type::Int8Ty));
    }

    std::string gname("_D");
    gname.append(cd->mangle());
    gname.append("12__destructorMFZv");

    llvm::Function* func = new llvm::Function(fnTy, llvm::GlobalValue::InternalLinkage, gname, gIR->module);
    llvm::Value* thisptr = func->arg_begin();
    thisptr->setName("this");

    llvm::BasicBlock* bb = new llvm::BasicBlock("entry", func);
    LLVMBuilder builder(bb);

    for (size_t i = 0; i < cd->dtors.dim; i++)
    {
        DtorDeclaration *d = (DtorDeclaration *)cd->dtors.data[i];
        DtoForceDeclareDsymbol(d);
        assert(d->irFunc->func);
        builder.CreateCall(d->irFunc->func, thisptr);
    }
    builder.CreateRetVoid();

    return llvm::ConstantExpr::getBitCast(func, getPtrToType(llvm::Type::Int8Ty));
}

static uint build_classinfo_flags(ClassDeclaration* cd)
{
    // adapted from original dmd code
    uint flags = 0;
    //flags |= isCOMclass(); // IUnknown
    bool hasOffTi = false;
    if (cd->ctor) flags |= 8;
    for (ClassDeclaration *cd2 = cd; cd2; cd2 = cd2->baseClass)
    {
    if (cd2->members)
    {
        for (size_t i = 0; i < cd2->members->dim; i++)
        {
        Dsymbol *sm = (Dsymbol *)cd2->members->data[i];
        if (sm->isVarDeclaration()) // is this enough?
            hasOffTi = true;
        //printf("sm = %s %s\n", sm->kind(), sm->toChars());
        if (sm->hasPointers())
            goto L2;
        }
    }
    }
    flags |= 2;         // no pointers
L2:
    if (hasOffTi)
        flags |= 4;
    return flags;
}

void DtoDefineClassInfo(ClassDeclaration* cd)
{
//     The layout is:
//        {
//         void **vptr;
//         monitor_t monitor;
//         byte[] initializer;     // static initialization data
//         char[] name;        // class name
//         void *[] vtbl;
//         Interface[] interfaces;
//         ClassInfo *base;        // base class
//         void *destructor;
//         void *invariant;        // class invariant
//         uint flags;
//         void *deallocator;
//         OffsetTypeInfo[] offTi;
//         void *defaultConstructor;
//        }

    if (cd->irStruct->classDefined) return;
    cd->irStruct->classDefined = true;

    Logger::println("DtoDefineClassInfo(%s)", cd->toChars());
    LOG_SCOPE;

    assert(cd->type->ty == Tclass);
    assert(cd->irStruct->classInfo);

    TypeClass* cdty = (TypeClass*)cd->type;
    if (!cd->isInterfaceDeclaration() && !cd->isAbstract()) {
        assert(cd->irStruct->init);
        assert(cd->irStruct->constInit);
        assert(cd->irStruct->vtbl);
        assert(cd->irStruct->constVtbl);
    }

    // holds the list of initializers for llvm
    std::vector<llvm::Constant*> inits;

    ClassDeclaration* cinfo = ClassDeclaration::classinfo;
    DtoForceConstInitDsymbol(cinfo);
    assert(cinfo->irStruct->constInit);

    llvm::Constant* c;

    // own vtable
    c = cinfo->irStruct->constInit->getOperand(0);
    assert(c);
    inits.push_back(c);

    // monitor
    c = cinfo->irStruct->constInit->getOperand(1);
    inits.push_back(c);

    // byte[] init
    const llvm::Type* byteptrty = getPtrToType(llvm::Type::Int8Ty);
    if (cd->isInterfaceDeclaration() || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(2);
    }
    else {
        c = llvm::ConstantExpr::getBitCast(cd->irStruct->init, byteptrty);
        assert(!cd->irStruct->constInit->getType()->isAbstract());
        size_t initsz = getABITypeSize(cd->irStruct->constInit->getType());
        c = DtoConstSlice(DtoConstSize_t(initsz), c);
    }
    inits.push_back(c);

    // class name
    // from dmd
    char *name = cd->ident->toChars();
    size_t namelen = strlen(name);
    if (!(namelen > 9 && memcmp(name, "TypeInfo_", 9) == 0))
    {
        name = cd->toPrettyChars();
        namelen = strlen(name);
    }
    c = DtoConstString(name);
    inits.push_back(c);

    // vtbl array
    if (cd->isInterfaceDeclaration() || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(4);
    }
    else {
        const llvm::Type* byteptrptrty = getPtrToType(byteptrty);
        assert(!cd->irStruct->vtbl->getType()->isAbstract());
        c = llvm::ConstantExpr::getBitCast(cd->irStruct->vtbl, byteptrptrty);
        assert(!cd->irStruct->constVtbl->getType()->isAbstract());
        size_t vtblsz = cd->irStruct->constVtbl->getType()->getNumElements();
        c = DtoConstSlice(DtoConstSize_t(vtblsz), c);
    }
    inits.push_back(c);

    // interfaces array
    IrStruct* irstruct = cd->irStruct;
    if (cd->isInterfaceDeclaration() || !irstruct->interfaceInfos || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(5);
    }
    else {
        const llvm::Type* t = cinfo->irStruct->constInit->getOperand(5)->getType()->getContainedType(1);
        c = llvm::ConstantExpr::getBitCast(irstruct->interfaceInfos, t);
        size_t iisz = irstruct->interfaceInfosTy->getNumElements();
        c = DtoConstSlice(DtoConstSize_t(iisz), c);
    }
    inits.push_back(c);

    // base classinfo
    if (cd->baseClass && !cd->isInterfaceDeclaration() && !cd->isAbstract()) {
        DtoDeclareClassInfo(cd->baseClass);
        c = cd->baseClass->irStruct->classInfo;
        assert(c);
        inits.push_back(c);
    }
    else {
        // null
        c = cinfo->irStruct->constInit->getOperand(6);
        inits.push_back(c);
    }

    // destructor
    if (cd->isInterfaceDeclaration() || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(7);
    }
    else {
        c = build_class_dtor(cd);
    }
    inits.push_back(c);

    // invariant
    // TODO
    c = cinfo->irStruct->constInit->getOperand(8);
    inits.push_back(c);

    // uint flags
    if (cd->isInterfaceDeclaration() || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(9);
    }
    else {
        uint flags = build_classinfo_flags(cd);
        c = DtoConstUint(flags);
    }
    inits.push_back(c);

    // allocator
    // TODO
    c = cinfo->irStruct->constInit->getOperand(10);
    inits.push_back(c);

    // offset typeinfo
    if (cd->isInterfaceDeclaration() || cd->isAbstract()) {
        c = cinfo->irStruct->constInit->getOperand(11);
    }
    else {
        c = build_offti_array(cd, cinfo->irStruct->constInit->getOperand(11));
    }
    inits.push_back(c);

    // default constructor
    if (cd->defaultCtor && !cd->isInterfaceDeclaration() && !cd->isAbstract()) {
        DtoForceDeclareDsymbol(cd->defaultCtor);
        c = isaConstant(cd->defaultCtor->irFunc->func);
        const llvm::Type* toTy = cinfo->irStruct->constInit->getOperand(12)->getType();
        c = llvm::ConstantExpr::getBitCast(c, toTy);
    }
    else {
        c = cinfo->irStruct->constInit->getOperand(12);
    }
    inits.push_back(c);

    /*size_t n = inits.size();
    for (size_t i=0; i<n; ++i)
    {
        Logger::cout() << "inits[" << i << "]: " << *inits[i] << '\n';
    }*/

    // build the initializer
    const llvm::StructType* st = isaStruct(cinfo->irStruct->constInit->getType());
    llvm::Constant* finalinit = llvm::ConstantStruct::get(st, inits);
    //Logger::cout() << "built the classinfo initializer:\n" << *finalinit <<'\n';

    cd->irStruct->constClassInfo = finalinit;
    cd->irStruct->classInfo->setInitializer(finalinit);
}
