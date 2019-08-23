/* Copyright (c) 2001, Stanford University
 * All rights reserved.
 *
 * See the file LICENSE.txt for information on redistributing this software.
 */

#ifndef CR_STATE_BUFFEROBJECT_H
#define CR_STATE_BUFFEROBJECT_H

#include "cr_hash.h"
#include "state/cr_statetypes.h"

#if defined(WINDOWS)
#define STATE_APIENTRY __stdcall
#else
#define STATE_APIENTRY
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	CRbitvalue	dirty[CR_MAX_BITARRAY];
	CRbitvalue	arrayBinding[CR_MAX_BITARRAY];
	CRbitvalue	elementsBinding[CR_MAX_BITARRAY];
    CRbitvalue  packBinding[CR_MAX_BITARRAY];
    CRbitvalue  unpackBinding[CR_MAX_BITARRAY];
} CRBufferObjectBits;


/*
 * Buffer object, like a texture object, but encapsulates arbitrary
 * data (vertex, image, etc).
 */
typedef struct {
	GLuint refCount;
	GLuint id;
    GLuint hwid;
	GLenum usage;
	GLenum access;
	GLuint size;      /* buffer size in bytes */
	GLvoid *pointer;  /* only valid while buffer is mapped */
	GLvoid *data;     /* the buffer data, if retainBufferData is true */
    GLboolean bResyncOnRead; /* buffer data could be changed on server side, 
                                so we need to resync every time guest wants to read from it*/
	CRbitvalue dirty[CR_MAX_BITARRAY];  /* dirty data or state */
	GLintptrARB dirtyStart, dirtyLength; /* dirty region */
    /* bitfield representing the object usage. 1 means the object is used by the context with the given bitid */
    CRbitvalue             ctxUsage[CR_MAX_BITARRAY];
} CRBufferObject;

typedef struct {
	GLboolean retainBufferData;  /* should state tracker retain buffer data? */
	CRBufferObject *arrayBuffer;
	CRBufferObject *elementsBuffer;
    CRBufferObject *packBuffer;
    CRBufferObject *unpackBuffer;

	CRBufferObject *nullBuffer;  /* name = 0 */
    /** Attached state tracker. */
    PCRStateTracker pStateTracker;
} CRBufferObjectState;

DECLEXPORT(CRBufferObject *) crStateGetBoundBufferObject(GLenum target, CRBufferObjectState *b);
DECLEXPORT(GLboolean) crStateIsBufferBound(PCRStateTracker pState, GLenum target);
struct CRContext;
DECLEXPORT(GLboolean) crStateIsBufferBoundForCtx(struct CRContext *g, GLenum target);

DECLEXPORT(GLuint) STATE_APIENTRY crStateBufferHWIDtoID(PCRStateTracker pState, GLuint hwid);
DECLEXPORT(GLuint) STATE_APIENTRY crStateGetBufferHWID(PCRStateTracker pState, GLuint id);

DECLEXPORT(void) crStateRegBuffers(PCRStateTracker pState, GLsizei n, GLuint *buffers);
#ifdef __cplusplus
}
#endif

#endif /* CR_STATE_BUFFEROBJECT_H */
