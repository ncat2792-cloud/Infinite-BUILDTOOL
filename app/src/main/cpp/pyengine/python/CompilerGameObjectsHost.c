/* Foreign-handle fixture: these Node allocations are deliberately not
 * PyObjects. Exercise the actual bridge and every destination failure point. */
#include "CompilerEntry.h"
#include "CompilerGameObjects.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Node { unsigned refs; char kind; size_t count; struct Node** children; } Node;
typedef struct { size_t live, calls, fail, codes, unicode, integers, longs, floats, singletons, frozen; } State;
static Node* node(State* s, char kind, size_t count) {
    if (s->calls++ == s->fail) return NULL;
    Node* n = calloc(1, sizeof(*n)); assert(n);
    n->refs=1; n->kind=kind; n->count=count;
    if (count) { n->children=calloc(count, sizeof(Node*)); assert(n->children); }
    ++s->live; return n;
}
static void release(void* context, void* handle) {
    State* s=context; Node* n=handle; if (!n || --n->refs) return;
    for (size_t i=0;i<n->count;++i) release(s,n->children[i]);
    free(n->children); free(n); --s->live;
}
static void* bytes(void* c,const char* p,size_t n){ assert(p || !n);return node(c,'s',0); }
static void* unicode(void* c,const char* p,size_t n){ assert(p && n);++((State*)c)->unicode;return node(c,'u',0); }
static void* integer(void* c,int64_t v){(void)v;++((State*)c)->integers;return node(c,'i',0);}
static void* longDecimal(void* c,const char* v){assert(v && *v);++((State*)c)->longs;return node(c,'l',0);}
static void* floating(void* c,double v){(void)v;++((State*)c)->floats;return node(c,'f',0);}
static void* singleton(void* c,int v){assert(v>=0 && v<4);++((State*)c)->singletons;return node(c,'N',0);}
static void* tuple(void* c,size_t count){return node(c,'(',count);}
static int tupleSet(void* c,void* t,size_t index,void* child){
    Node* n=t; assert(n->kind=='(' && index<n->count && !n->children[index]);
    n->children[index]=child; return 0;
}
static void* frozen(void* c,void* values){
    ++((State*)c)->frozen; Node* n=node(c,'>',1);
    if(n){n->children[0]=values;++n->children[0]->refs;}return n;
}
static void* code(void* c,const PyLoaderPythonCodeFields* f){
    assert(f->argcount>=0 && f->nlocals>=0 && f->stacksize>0 && f->firstlineno>=1);
    Node* children[]={f->code,f->consts,f->names,f->varnames,f->freevars,f->cellvars,f->filename,f->name,f->lnotab};
    assert(children[0]->kind=='s' && children[1]->kind=='(' && children[2]->kind=='(');
    ++((State*)c)->codes; Node* n=node(c,'c',9);
    if(n)for(size_t i=0;i<9;++i){n->children[i]=children[i];++children[i]->refs;}return n;
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    unsigned char* initial=NULL;size_t initialSize=0;char* error=NULL;
    assert(PyLoaderCompilePython3105("pass\n","<init>",&initial,&initialSize,&error));free(initial);
    FILE* file=fopen(argv[1],"rb");if(!file)return 3;
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0 && size<=16*1024*1024);rewind(file);
    char* data=malloc((size_t)size);assert(data && fread(data,1,(size_t)size,file)==(size_t)size);fclose(file);
    State s={0};s.fail=(size_t)-1;
    PyLoaderPythonConstructors api={&s,bytes,unicode,integer,longDecimal,floating,singleton,tuple,tupleSet,frozen,code,release};
    Node* result=PyLoaderBuildPython3105(data,(size_t)size,&api,&error);
    if(!result){fprintf(stderr,"bridge failed: %s\n",error?error:"destination");return 4;}
    assert(!error && result->kind=='c' && s.codes>=2 && s.unicode && s.integers && s.longs && s.floats && s.singletons);
    const size_t calls=s.calls;
    release(&s,result);assert(!s.live);
    for(size_t failure=0;failure<calls;++failure){
        memset(&s,0,sizeof(s));s.fail=failure;
        result=PyLoaderBuildPython3105(data,(size_t)size,&api,&error);
        assert(!result && !error && !s.live);
    }
    free(data);PyLoaderFinalizePythonCompiler();
    printf("PASS actual foreign-object bridge, nested code/constants, %zu allocation failure retirements\n",calls);
    return 0;
}
