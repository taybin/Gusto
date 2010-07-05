/*
 *  objcsigcglue.h
 *  Gusto
 *
 *  Created by Taybin on 8/25/06.
 *  Copyright 2006-2007 Penguin Sounds. All rights reserved.
 *
 */

/*
 
 Example:
 
 SignalActivated.connect (sigc::bind(sigc::ptr_fun(objcsigcglue<bool>), self, @selector(CatchSignal:)));
 
 This will call [self CatchSignal:boolValue]. Pretty neat, right? For the majority of callbacks that 
 don’t accept arguments, a mere function pointer cast is needed:
 
 BoringSignal.connect (sigc::bind(sigc::ptr_fun((msgFtn)objc_msgSend), self, @selector(boringSignalCatch:)));

 */ 

#ifndef __cocoa_glue_h__
#define __cocoa_glue_h__

#import <objc/objc-runtime.h>

typedef void (*msgFtn)(id, SEL);

template <typename T1>
void
objcsigcglue (T1 t1, id _id, SEL _sel)
{
	void (*voidReturn)(id, SEL, T1) = (void (*)(id, SEL, T1))objc_msgSend;

	voidReturn(_id, _sel, t1);
}

template <typename T1, typename T2>
void
objcsigcgluebind (T1 t1, id _id, SEL _sel, T2 t2)
{
	void (*voidReturn)(id, SEL, T1, T2) = (void (*)(id, SEL, T1, T2))objc_msgSend;
	
	voidReturn(_id, _sel, t1, t2);
}

template <typename T1, typename T2>
void
objcsigcglue2 (T1 t1, T2 t2, id _id, SEL _sel)
{
	void (*voidReturn)(id, SEL, T1, T2) = (void (*)(id, SEL, T1, T2))objc_msgSend;

	voidReturn(_id, _sel, t1, t2);
}

template <typename T1, typename T2, typename T3>
void
objcsigcglue2bind (T1 t1, T2 t2, id _id, SEL _sel, T3 t3)
{
	void (*voidReturn)(id, SEL, T1, T2, T3) = (void (*)(id, SEL, T1, T2, T3))objc_msgSend;

	voidReturn(_id, _sel, t1, t2, t3);
}

typedef int (*intMsgFtn)(id, SEL);

template <typename T1>
int
intobjcsigcglue (T1 t1, id _id, SEL _sel)
{
	int (*intReturn)(id, SEL, T1) = (int (*)(id, SEL, T1))objc_msgSend;
	
	return intReturn(_id, _sel, t1);
}

template <typename T1, typename T2>
int
intobjcsigcgluebind (T1 t1, id _id, SEL _sel, T2 t2)
{
	int (*intReturn)(id, SEL, T1, T2) = (int (*)(id, SEL, T1, T2))objc_msgSend;
	
	return intReturn(_id, _sel, t1, t2);
}

#endif // __cocoa_glue_h__
