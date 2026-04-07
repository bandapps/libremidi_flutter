

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 8.01.0628 */
/* at Mon Jan 18 22:14:07 2038
 */
/* Compiler settings for WindowsMidiServicesAppSdkComExtensions.idl:
    Oicf, W1, Zp8, env=Win64 (32b run), target_arch=AMD64 8.01.0628 
    protocol : all , ms_ext, c_ext, robust
    error checks: allocation ref bounds_check enum stub_data 
    VC __declspec() decoration level: 
         __declspec(uuid()), __declspec(selectany), __declspec(novtable)
         DECLSPEC_UUID(), MIDL_INTERFACE()
*/
/* @@MIDL_FILE_HEADING(  ) */



/* verify that the <rpcndr.h> version is high enough to compile this file*/
#ifndef __REQUIRED_RPCNDR_H_VERSION__
#define __REQUIRED_RPCNDR_H_VERSION__ 500
#endif

#include "rpc.h"
#include "rpcndr.h"

#ifndef __RPCNDR_H_VERSION__
#error this stub requires an updated version of <rpcndr.h>
#endif /* __RPCNDR_H_VERSION__ */

#ifndef COM_NO_WINDOWS_H
#include "windows.h"
#include "ole2.h"
#endif /*COM_NO_WINDOWS_H*/

#ifndef __WindowsMidiServicesAppSdkComExtensions_h__
#define __WindowsMidiServicesAppSdkComExtensions_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#ifndef DECLSPEC_XFGVIRT
#if defined(_CONTROL_FLOW_GUARD_XFG)
#define DECLSPEC_XFGVIRT(base, func) __declspec(xfg_virtual(base, func))
#else
#define DECLSPEC_XFGVIRT(base, func)
#endif
#endif

/* Forward Declarations */ 

#ifndef __IMidiEndpointConnectionMessagesReceivedCallback_FWD_DEFINED__
#define __IMidiEndpointConnectionMessagesReceivedCallback_FWD_DEFINED__
typedef interface IMidiEndpointConnectionMessagesReceivedCallback IMidiEndpointConnectionMessagesReceivedCallback;

#endif 	/* __IMidiEndpointConnectionMessagesReceivedCallback_FWD_DEFINED__ */


#ifndef __IMidiEndpointConnectionRaw_FWD_DEFINED__
#define __IMidiEndpointConnectionRaw_FWD_DEFINED__
typedef interface IMidiEndpointConnectionRaw IMidiEndpointConnectionRaw;

#endif 	/* __IMidiEndpointConnectionRaw_FWD_DEFINED__ */


/* header files for imported files */
#include "unknwn.h"
#include "oaidl.h"
#include "ocidl.h"

#ifdef __cplusplus
extern "C"{
#endif 


#ifndef __IMidiEndpointConnectionMessagesReceivedCallback_INTERFACE_DEFINED__
#define __IMidiEndpointConnectionMessagesReceivedCallback_INTERFACE_DEFINED__

/* interface IMidiEndpointConnectionMessagesReceivedCallback */
/* [uuid][local][object] */ 


EXTERN_C const IID IID_IMidiEndpointConnectionMessagesReceivedCallback;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("8087b303-0519-31d1-31d1-000000000010")
    IMidiEndpointConnectionMessagesReceivedCallback : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE MessagesReceived( 
            /* [annotation][in] */ 
            _In_  GUID sessionId,
            /* [annotation][in] */ 
            _In_  GUID connectionId,
            /* [annotation][in] */ 
            _In_  UINT64 timestamp,
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *messages) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct IMidiEndpointConnectionMessagesReceivedCallbackVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            IMidiEndpointConnectionMessagesReceivedCallback * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            IMidiEndpointConnectionMessagesReceivedCallback * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            IMidiEndpointConnectionMessagesReceivedCallback * This);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionMessagesReceivedCallback, MessagesReceived)
        HRESULT ( STDMETHODCALLTYPE *MessagesReceived )( 
            IMidiEndpointConnectionMessagesReceivedCallback * This,
            /* [annotation][in] */ 
            _In_  GUID sessionId,
            /* [annotation][in] */ 
            _In_  GUID connectionId,
            /* [annotation][in] */ 
            _In_  UINT64 timestamp,
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *messages);
        
        END_INTERFACE
    } IMidiEndpointConnectionMessagesReceivedCallbackVtbl;

    interface IMidiEndpointConnectionMessagesReceivedCallback
    {
        CONST_VTBL struct IMidiEndpointConnectionMessagesReceivedCallbackVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define IMidiEndpointConnectionMessagesReceivedCallback_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define IMidiEndpointConnectionMessagesReceivedCallback_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define IMidiEndpointConnectionMessagesReceivedCallback_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define IMidiEndpointConnectionMessagesReceivedCallback_MessagesReceived(This,sessionId,connectionId,timestamp,wordCount,messages)	\
    ( (This)->lpVtbl -> MessagesReceived(This,sessionId,connectionId,timestamp,wordCount,messages) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* __IMidiEndpointConnectionMessagesReceivedCallback_INTERFACE_DEFINED__ */


#ifndef __IMidiEndpointConnectionRaw_INTERFACE_DEFINED__
#define __IMidiEndpointConnectionRaw_INTERFACE_DEFINED__

/* interface IMidiEndpointConnectionRaw */
/* [uuid][local][object] */ 


EXTERN_C const IID IID_IMidiEndpointConnectionRaw;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("8087b303-0519-31d1-31d1-000000000020")
    IMidiEndpointConnectionRaw : public IUnknown
    {
    public:
        virtual UINT32 STDMETHODCALLTYPE GetSupportedMaxMidiWordsPerTransmission( void) = 0;
        
        virtual BOOL STDMETHODCALLTYPE ValidateBufferHasOnlyCompleteUmps( 
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *messages) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SendMidiMessagesRaw( 
            /* [annotation][in] */ 
            _In_  UINT64 timestamp,
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *completeMessages) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE SetMessagesReceivedCallback( 
            /* [annotation][in] */ 
            _In_  IMidiEndpointConnectionMessagesReceivedCallback *messagesReceivedCallback) = 0;
        
        virtual HRESULT STDMETHODCALLTYPE RemoveMessagesReceivedCallback( void) = 0;
        
    };
    
    
#else 	/* C style interface */

    typedef struct IMidiEndpointConnectionRawVtbl
    {
        BEGIN_INTERFACE
        
        DECLSPEC_XFGVIRT(IUnknown, QueryInterface)
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            IMidiEndpointConnectionRaw * This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */ 
            _COM_Outptr_  void **ppvObject);
        
        DECLSPEC_XFGVIRT(IUnknown, AddRef)
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            IMidiEndpointConnectionRaw * This);
        
        DECLSPEC_XFGVIRT(IUnknown, Release)
        ULONG ( STDMETHODCALLTYPE *Release )( 
            IMidiEndpointConnectionRaw * This);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionRaw, GetSupportedMaxMidiWordsPerTransmission)
        UINT32 ( STDMETHODCALLTYPE *GetSupportedMaxMidiWordsPerTransmission )( 
            IMidiEndpointConnectionRaw * This);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionRaw, ValidateBufferHasOnlyCompleteUmps)
        BOOL ( STDMETHODCALLTYPE *ValidateBufferHasOnlyCompleteUmps )( 
            IMidiEndpointConnectionRaw * This,
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *messages);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionRaw, SendMidiMessagesRaw)
        HRESULT ( STDMETHODCALLTYPE *SendMidiMessagesRaw )( 
            IMidiEndpointConnectionRaw * This,
            /* [annotation][in] */ 
            _In_  UINT64 timestamp,
            /* [annotation][in] */ 
            _In_  UINT32 wordCount,
            /* [annotation][in] */ 
            _In_  UINT32 *completeMessages);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionRaw, SetMessagesReceivedCallback)
        HRESULT ( STDMETHODCALLTYPE *SetMessagesReceivedCallback )( 
            IMidiEndpointConnectionRaw * This,
            /* [annotation][in] */ 
            _In_  IMidiEndpointConnectionMessagesReceivedCallback *messagesReceivedCallback);
        
        DECLSPEC_XFGVIRT(IMidiEndpointConnectionRaw, RemoveMessagesReceivedCallback)
        HRESULT ( STDMETHODCALLTYPE *RemoveMessagesReceivedCallback )( 
            IMidiEndpointConnectionRaw * This);
        
        END_INTERFACE
    } IMidiEndpointConnectionRawVtbl;

    interface IMidiEndpointConnectionRaw
    {
        CONST_VTBL struct IMidiEndpointConnectionRawVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define IMidiEndpointConnectionRaw_QueryInterface(This,riid,ppvObject)	\
    ( (This)->lpVtbl -> QueryInterface(This,riid,ppvObject) ) 

#define IMidiEndpointConnectionRaw_AddRef(This)	\
    ( (This)->lpVtbl -> AddRef(This) ) 

#define IMidiEndpointConnectionRaw_Release(This)	\
    ( (This)->lpVtbl -> Release(This) ) 


#define IMidiEndpointConnectionRaw_GetSupportedMaxMidiWordsPerTransmission(This)	\
    ( (This)->lpVtbl -> GetSupportedMaxMidiWordsPerTransmission(This) ) 

#define IMidiEndpointConnectionRaw_ValidateBufferHasOnlyCompleteUmps(This,wordCount,messages)	\
    ( (This)->lpVtbl -> ValidateBufferHasOnlyCompleteUmps(This,wordCount,messages) ) 

#define IMidiEndpointConnectionRaw_SendMidiMessagesRaw(This,timestamp,wordCount,completeMessages)	\
    ( (This)->lpVtbl -> SendMidiMessagesRaw(This,timestamp,wordCount,completeMessages) ) 

#define IMidiEndpointConnectionRaw_SetMessagesReceivedCallback(This,messagesReceivedCallback)	\
    ( (This)->lpVtbl -> SetMessagesReceivedCallback(This,messagesReceivedCallback) ) 

#define IMidiEndpointConnectionRaw_RemoveMessagesReceivedCallback(This)	\
    ( (This)->lpVtbl -> RemoveMessagesReceivedCallback(This) ) 

#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* __IMidiEndpointConnectionRaw_INTERFACE_DEFINED__ */


/* Additional Prototypes for ALL interfaces */

/* end of Additional Prototypes */

#ifdef __cplusplus
}
#endif

#endif


