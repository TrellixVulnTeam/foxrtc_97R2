/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 2.0.6
 * 
 * This file is not intended to be easily readable and contains a number of 
 * coding conventions designed to improve portability and efficiency. Do not make
 * changes to this file unless you know what you are doing--modify the SWIG 
 * interface file instead. 
 * ----------------------------------------------------------------------------- */

#import <Foundation/Foundation.h>


#import "foxrtc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

@interface FoxrtcTransport : NSObject
{
	void *swigCPtr;
	BOOL swigCMemOwn;
}
-(void*)getCptr;
-(id)initWithCptr: (void*)cptr swigOwnCObject: (BOOL)ownCObject;
-(id)init;
-(int)SendRtp: (NSString *)data len: (int)len;
-(int)SendRtcp: (NSString *)data len: (int)len;

-(void)dealloc;

@end

@interface Foxrtc : NSObject
{
	void *swigCPtr;
	BOOL swigCMemOwn;
}
-(void*)getCptr;
-(id)initWithCptr: (void*)cptr swigOwnCObject: (BOOL)ownCObject;
+(Foxrtc*)Instance;
-(int)Init: (FoxrtcTransport*)transport;
-(int)Uninit;
-(int)GetDeviceInfo;
-(int)OpenCamera: (int)index;
-(int)CloseCamera;
-(int)CreateLocalAudioStream: (unsigned int)ssrc;
-(int)DeleteLocalAudioStream;
-(int)CreateRemoteAudioStream: (unsigned int)ssrc;
-(int)DeleteRemoteAudioStream;
-(int)CreateLocalVideoStream: (int)ssrc view: (RenderView*)view;
-(int)DeleteLocalVideoStream;
-(int)CreateRemoteVideoStream: (int)ssrc view: (RenderView*)view;
-(int)DeleteRemoteVideoStream;
-(int)IncomingData: (NSString *)data len: (int)len;

-(void)dealloc;

@end


#ifdef __cplusplus
}
#endif

