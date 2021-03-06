#ifndef GLP_OBJECT_H_
#define GLP_OBJECT_H_

#include "log.h"
#include <list>
#include <new>
#include <stdarg.h>

/*
内存池，顶级对象申明，
*/

namespace Tool{
	class Mutex;
	class Object;

	typedef std::list<Object*> LISTOBJS;
	//本库对象管理器
	class ObjectMgr{
	private:
		ObjectMgr();
		virtual ~ObjectMgr();
	public:
		static ObjectMgr* GetInstance();

		void addObj(Object* obj);
		void removeObj(Object* obj);

		void clearObjs();

	private:
		LISTOBJS objs;
		Mutex* mutex;
	};

	//本库基础对象继承Object
	class Object{
	private:
		Object(const char* name=0);
		virtual ~Object();

	public:
		/*hold住
		@return ref;
		*/
		int hold();
		/*释放 
		@return ref;
		*/
		int release();

		//托管给ObjectMgr
		void hosting();

	private:
		char name[200];
		int ref;
		ObjectMgr* mgr;
	};

	//内存分配管理器
	class Allocator{
	private:
		virtual ~Allocator(){}

	public:
		/*
		@size  期望分配的内存大小
		*/
		virtual void* alloc(size_t size);
		virtual void dealloc(void* pointer);

		static Allocator* GetInstance();
	};
}

#define calloc_(M_o) \
	Allocator::GetInstance()->alloc(M_o)

#define free_(M_o) \
	Allocator::GetInstance()->dealloc((void*)M_o)

//通用对象的 new_和delete_，可以不用继承Object
#define new_(M_T,M_o,...) \
	M_T* M_o = (M_T*)Tool::Allocator::GetInstance()->alloc(sizeof(M_T)); \
	M_o = M_o == NULL ? NULL : new (M_o) M_T (__VA_ARGS__)

#define delete_(M_T,M_o)\
	if (M_o != NULL){\
	M_o->~M_T();\
	Tool::Allocator::GetInstance()->dealloc(M_o);\
	M_o = NULL;\
	}

#endif//GLP_OBJECT_H_
