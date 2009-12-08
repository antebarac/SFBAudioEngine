/*
 *  Copyright (C) 2006, 2007, 2008, 2009 Stephen F. Booth <me@sbooth.org>
 *  All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    - Neither the name of Stephen F. Booth nor the names of its 
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <libkern/OSAtomic.h>
#include <pthread.h>
#include <mach/thread_act.h>
#include <mach/mach_error.h>
#include <mach/task.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <stdexcept>

#include "AudioEngineDefines.h"
#include "AudioPlayer.h"
#include "AudioDecoder.h"
#include "DecoderStateData.h"

#include "CARingBuffer.h"


// ========================================
// Macros
// ========================================
#define RING_BUFFER_SIZE_FRAMES					16384
#define RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES		2048
#define FEEDER_THREAD_IMPORTANCE				6


// ========================================
// Utility functions
// ========================================
static bool
channelLayoutsAreEqual(AudioChannelLayout *lhs,
					   AudioChannelLayout *rhs)
{
	assert(NULL != lhs);
	assert(NULL != rhs);
	
	// First check if the tags are equal
	if(lhs->mChannelLayoutTag != rhs->mChannelLayoutTag)
		return false;
	
	// If the tags are equal, check for special values
	if(kAudioChannelLayoutTag_UseChannelBitmap == lhs->mChannelLayoutTag)
		return (lhs->mChannelBitmap == rhs->mChannelBitmap);
	
	if(kAudioChannelLayoutTag_UseChannelDescriptions == lhs->mChannelLayoutTag) {
		if(lhs->mNumberChannelDescriptions != rhs->mNumberChannelDescriptions)
			return false;
		
		size_t bytesToCompare = lhs->mNumberChannelDescriptions * sizeof(AudioChannelDescription);
		return (0 == memcmp(&lhs->mChannelDescriptions, &rhs->mChannelDescriptions, bytesToCompare));
	}
	
	return true;
}

// ========================================
// Set the calling thread's timesharing and importance
// ========================================
static bool
setThreadPolicy(integer_t importance)
{
	// Turn off timesharing
	thread_extended_policy_data_t extendedPolicy = { 0 };
	kern_return_t error = thread_policy_set(mach_thread_self(),
											THREAD_EXTENDED_POLICY,
											(thread_policy_t)&extendedPolicy, 
											THREAD_EXTENDED_POLICY_COUNT);
	
	if(KERN_SUCCESS != error) {
#if DEBUG
		mach_error(const_cast<char *>("Couldn't set thread's extended policy"), error);
#endif
		return false;
	}
	
	// Give the thread the specified importance
	thread_precedence_policy_data_t precedencePolicy = { importance };
	error = thread_policy_set(mach_thread_self(), 
							  THREAD_PRECEDENCE_POLICY, 
							  (thread_policy_t)&precedencePolicy, 
							  THREAD_PRECEDENCE_POLICY_COUNT);
	
	if (error != KERN_SUCCESS) {
#if DEBUG
		mach_error(const_cast<char *>("Couldn't set thread's precedence policy"), error);
#endif
		return false;
	}
	
	return true;
}

// ========================================
// The AUGraph input callback
// ========================================
static OSStatus
myAURenderCallback(void *							inRefCon,
				   AudioUnitRenderActionFlags *		ioActionFlags,
				   const AudioTimeStamp *			inTimeStamp,
				   UInt32							inBusNumber,
				   UInt32							inNumberFrames,
				   AudioBufferList *				ioData)
{
	assert(NULL != inRefCon);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(inRefCon);
	return player->Render(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
}

static OSStatus
auGraphDidRender(void *							inRefCon,
				 AudioUnitRenderActionFlags *	ioActionFlags,
				 const AudioTimeStamp *			inTimeStamp,
				 UInt32							inBusNumber,
				 UInt32							inNumberFrames,
				 AudioBufferList *				ioData)
{
	assert(NULL != inRefCon);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(inRefCon);
	return player->DidRender(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
}

// ========================================
// The file reader thread's entry point
// ========================================
static void *
fileReaderEntry(void *arg)
{
	assert(NULL != arg);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(arg);
	return player->FileReaderThreadEntry();
}

// ========================================
// The collector thread's entry point
// ========================================
static void *
collectorEntry(void *arg)
{
	assert(NULL != arg);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(arg);
	return player->CollectorThreadEntry();
}


#pragma mark Creation/Destruction


AudioPlayer::AudioPlayer()
	: mDecoderQueue(), mRingBuffer(NULL), mFramesDecoded(0), mFramesRendered(0), mNextDecoderStartingTimeStamp(0)
{
	mRingBuffer = new CARingBuffer();

	// Create the semaphore and mutex to be used by the decoding and rendering threads
	kern_return_t result = semaphore_create(mach_task_self(), &mDecoderSemaphore, SYNC_POLICY_FIFO, 0);
	if(KERN_SUCCESS != result) {
#if DEBUG
		mach_error(const_cast<char *>("semaphore_create"), result);
#endif

		delete mRingBuffer, mRingBuffer = NULL;

		throw std::runtime_error("semaphore_create failed");
	}

	result = semaphore_create(mach_task_self(), &mCollectorSemaphore, SYNC_POLICY_FIFO, 0);
	if(KERN_SUCCESS != result) {
#if DEBUG
		mach_error(const_cast<char *>("semaphore_create"), result);
#endif
		
		delete mRingBuffer, mRingBuffer = NULL;

		result = semaphore_destroy(mach_task_self(), mDecoderSemaphore);
#if DEBUG
		if(KERN_SUCCESS != result)
			mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif
		
		throw std::runtime_error("semaphore_create failed");
	}
	
	int success = pthread_mutex_init(&mMutex, NULL);
	if(0 != success) {
		ERR("pthread_mutex_init failed: %i", success);
		
		delete mRingBuffer, mRingBuffer = NULL;
		
		result = semaphore_destroy(mach_task_self(), mDecoderSemaphore);
#if DEBUG
		if(KERN_SUCCESS != result)
			mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif

		result = semaphore_destroy(mach_task_self(), mCollectorSemaphore);
#if DEBUG
		if(KERN_SUCCESS != result)
			mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif

		throw std::runtime_error("pthread_mutex_init failed");
	}

	for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i)
		mActiveDecoders[i] = NULL;
	
	// Launch the collector thread
	mKeepCollecting = true;
	int creationResult = pthread_create(&mCollectorThread, NULL, collectorEntry, this);
	if(0 != creationResult) {
		ERR("pthread_create failed: %i", creationResult);
		
		delete mRingBuffer, mRingBuffer = NULL;
		
		result = semaphore_destroy(mach_task_self(), mDecoderSemaphore);
#if DEBUG
		if(KERN_SUCCESS != result)
			mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif
		
		result = semaphore_destroy(mach_task_self(), mCollectorSemaphore);
#if DEBUG
		if(KERN_SUCCESS != result)
			mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif
		
		throw std::runtime_error("pthread_create failed");
	}
	
	// Set up our AUGraph and set pregain to 0
	OSStatus status = CreateAUGraph();
	if(noErr != status) {
		ERR("CreateAUGraph failed: %i", result);
	}
	
	if(false == SetPreGain(0))
		ERR("SetPreGain failed");
}

AudioPlayer::~AudioPlayer()
{
	// Stop the processing graph and reclaim its resources
	DisposeAUGraph();

	// Dispose of all active decoders
	StopActiveDecoders();
	
	// End the collector thread
	mKeepCollecting = false;
	semaphore_signal(mCollectorSemaphore);
	
	int joinResult = pthread_join(mCollectorThread, NULL);
	if(0 != joinResult)
		ERR("pthread_join failed: %i", joinResult);
	
	mCollectorThread = static_cast<pthread_t>(0);

	// Force any decoders left hanging by the collector to end
	for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
		if(NULL != mActiveDecoders[i])
			delete mActiveDecoders[i], mActiveDecoders[i] = NULL;
	}
	
	// Clean up any queued decoders
	while(false == mDecoderQueue.empty()) {
		AudioDecoder *decoder = mDecoderQueue.front();
		mDecoderQueue.pop_front();
		delete decoder;
	}

	// Clean up the ring buffer
	if(mRingBuffer)
		delete mRingBuffer, mRingBuffer = NULL;
	
	// Destroy the decoder and collector semaphores
	kern_return_t result = semaphore_destroy(mach_task_self(), mDecoderSemaphore);
#if DEBUG
	if(KERN_SUCCESS != result)
		mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif

	result = semaphore_destroy(mach_task_self(), mCollectorSemaphore);
#if DEBUG
	if(KERN_SUCCESS != result)
		mach_error(const_cast<char *>("semaphore_destroy"), result);
#endif
	
	// Destroy the decoder mutex
	int success = pthread_mutex_destroy(&mMutex);
	if(0 != success)
		ERR("pthread_mutex_destroy failed: %i", success);
}


#pragma mark Playback Control


void AudioPlayer::Play()
{
	if(IsPlaying())
		return;

	OSStatus result = AUGraphStart(mAUGraph);

	if(noErr != result)
		ERR("AUGraphStart failed: %i", result);
}

void AudioPlayer::Pause()
{
	if(!IsPlaying())
		return;
	
	OSStatus result = AUGraphStop(mAUGraph);

	if(noErr != result)
		ERR("AUGraphStop failed: %i", result);
}

void AudioPlayer::Stop()
{
	if(!IsPlaying())
		return;

	Pause();
	
	StopActiveDecoders();
	ResetAUGraph();
	
	mFramesDecoded = 0;
	mFramesRendered = 0;
	mNextDecoderStartingTimeStamp = 0;
}

bool AudioPlayer::IsPlaying()
{
	Boolean isRunning = FALSE;
	OSStatus result = AUGraphIsRunning(mAUGraph, &isRunning);

	if(noErr != result)
		ERR("AUGraphIsRunning failed: %i", result);
		
	return isRunning;
}


#pragma mark Playback Properties


SInt64 AudioPlayer::GetCurrentFrame()
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return -1;
	
	return (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
}

SInt64 AudioPlayer::GetTotalFrames()
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return -1;
	
	return currentDecoderState->mTotalFrames;
}

CFTimeInterval AudioPlayer::GetCurrentTime()
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return -1;
	
	return static_cast<CFTimeInterval>(GetCurrentFrame() / currentDecoderState->mDecoder->GetFormat().mSampleRate);
}

CFTimeInterval AudioPlayer::GetTotalTime()
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return -1;
	
	return static_cast<CFTimeInterval>(currentDecoderState->mTotalFrames / currentDecoderState->mDecoder->GetFormat().mSampleRate);
}


#pragma mark Seeking


bool AudioPlayer::SeekForward(CFTimeInterval secondsToSkip)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;

	SInt64 frameCount		= static_cast<SInt64>(secondsToSkip * currentDecoderState->mDecoder->GetFormat().mSampleRate);	
	SInt64 desiredFrame		= GetCurrentFrame() + frameCount;
	SInt64 totalFrames		= currentDecoderState->mTotalFrames;
	
	return SeekToFrame(std::min(desiredFrame, totalFrames));
}

bool AudioPlayer::SeekBackward(CFTimeInterval secondsToSkip)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;

	SInt64 frameCount		= static_cast<SInt64>(secondsToSkip * currentDecoderState->mDecoder->GetFormat().mSampleRate);	
	SInt64 currentFrame		= GetCurrentFrame();
	SInt64 desiredFrame		= currentFrame - frameCount;
	
	return SeekToFrame(std::max(0LL, desiredFrame));
}

bool AudioPlayer::SeekToTime(CFTimeInterval timeInSeconds)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;
	
	SInt64 desiredFrame		= static_cast<SInt64>(timeInSeconds * currentDecoderState->mDecoder->GetFormat().mSampleRate);	
	SInt64 totalFrames		= currentDecoderState->mTotalFrames;
	
	return SeekToFrame(std::max(0LL, std::min(desiredFrame, totalFrames)));
}

bool AudioPlayer::SeekToFrame(SInt64 frame)
{
	assert(0 <= frame);

	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;
	
	if(false == currentDecoderState->mDecoder->SupportsSeeking())
		return false;
	
//	Float64 graphLatency = GetAUGraphLatency();
//	if(-1 != graphLatency) {
//		SInt64 graphLatencyFrames = static_cast<SInt64>(graphLatency * mAUGraphFormat.mSampleRate);
//		frame -= graphLatencyFrames;
//	}
	
	if(false == OSAtomicCompareAndSwap64Barrier(currentDecoderState->mFrameToSeek, frame, &currentDecoderState->mFrameToSeek))
		return false;
	
	semaphore_signal(mDecoderSemaphore);

	return true;	
}


#pragma mark Player Parameters


Float32 AudioPlayer::GetVolume()
{
	AudioUnit au = NULL;
	OSStatus auResult = AUGraphNodeInfo(mAUGraph, 
										mOutputNode, 
										NULL, 
										&au);

	if(noErr != auResult) {
		ERR("AUGraphNodeInfo failed: %i", auResult);
		return -1;
	}
	
	Float32 volume = -1;
	ComponentResult result = AudioUnitGetParameter(au,
												   kHALOutputParam_Volume,
												   kAudioUnitScope_Global,
												   0,
												   &volume);
	
	if(noErr != result)
		ERR("AudioUnitGetParameter (kHALOutputParam_Volume) failed: %i", result);
		
	return volume;
}

bool AudioPlayer::SetVolume(Float32 volume)
{
	assert(0 <= volume);
	assert(volume <= 1);
	
	AudioUnit au = NULL;
	OSStatus auResult = AUGraphNodeInfo(mAUGraph, 
										mOutputNode, 
										NULL, 
										&au);
	
	if(noErr != auResult) {
		ERR("AUGraphNodeInfo failed: %i", auResult);
		return -1;
	}
	
	ComponentResult result = AudioUnitSetParameter(au,
												   kHALOutputParam_Volume,
												   kAudioUnitScope_Global,
												   0,
												   volume,
												   0);

	if(noErr != result) {
		ERR("AudioUnitSetParameter (kHALOutputParam_Volume) failed: %i", result);
		return false;
	}
	
	return true;
}

Float32 AudioPlayer::GetPreGain()
{
	if(false == IsPreGainEnabled())
		return 0.f;

	AudioUnit au = NULL;
	OSStatus auResult = AUGraphNodeInfo(mAUGraph, 
										mLimiterNode, 
										NULL, 
										&au);
	
	if(noErr != auResult) {
		ERR("AUGraphNodeInfo failed: %i", auResult);
		return -1;
	}

	Float32 preGain = -1;
	ComponentResult result = AudioUnitGetParameter(au, 
												   kLimiterParam_PreGain, 
												   kAudioUnitScope_Global, 
												   0,
												   &preGain);
	
	if(noErr != result)
		ERR("AudioUnitGetParameter (kLimiterParam_PreGain) failed: %i", result);
	
	return preGain;
}

bool AudioPlayer::SetPreGain(Float32 preGain)
{
	if(0.f == preGain)
		return EnablePreGain(false);
	
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mLimiterNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	AudioUnitParameter auParameter;
	
	auParameter.mAudioUnit		= au;
	auParameter.mParameterID	= kLimiterParam_PreGain;
	auParameter.mScope			= kAudioUnitScope_Global;
	auParameter.mElement		= 0;
	
	result	= AUParameterSet(NULL, 
							 NULL, 
							 &auParameter, 
							 preGain,
							 0);
	
	if(noErr != result) {
		ERR("AUParameterSet (kLimiterParam_PreGain) failed: %i", result);
		return false;
	}
	
	return true;
}


#pragma mark DSP Effects


bool AudioPlayer::AddEffect(OSType subType, OSType manufacturer, UInt32 flags, UInt32 mask, AudioUnit *effectUnit1)
{
	// Get the source node for the graph's output node
	UInt32 numInteractions = 0;
	OSStatus result = AUGraphCountNodeInteractions(mAUGraph, 
												   mOutputNode, 
												   &numInteractions);
	if(noErr != result) {
		ERR("AUGraphCountNodeConnections failed: %i", result);
		return false;
	}
	
	AUNodeInteraction *interactions = static_cast<AUNodeInteraction *>(calloc(numInteractions, sizeof(AUNodeInteraction)));
	if(NULL == interactions) {
		ERR("Unable to allocate memory");
		return false;
	}
	
	result = AUGraphGetNodeInteractions(mAUGraph, 
										mOutputNode,
										&numInteractions, 
										interactions);
	
	if(noErr != result) {
		ERR("AUGraphGetNodeInteractions failed: %i", result);
		
		free(interactions), interactions = NULL;
		
		return false;
	}
	
	AUNode sourceNode = -1;
	for(UInt32 interactionIndex = 0; interactionIndex < numInteractions; ++interactionIndex) {
		AUNodeInteraction interaction = interactions[interactionIndex];
		
		if(kAUNodeInteraction_Connection == interaction.nodeInteractionType && mOutputNode == interaction.nodeInteraction.connection.destNode) {
			sourceNode = interaction.nodeInteraction.connection.sourceNode;
			break;
		}
	}						
	
	free(interactions), interactions = NULL;

	// Unable to determine the preceding node, so bail
	if(-1 == sourceNode) {
		ERR("Unable to determine input node");
		return false;
	}
	
	// Create the effect node and set its format
	ComponentDescription desc = { kAudioUnitType_Effect, subType, manufacturer, flags, mask };
	
	AUNode effectNode = -1;
	result = AUGraphAddNode(mAUGraph, 
							&desc, 
							&effectNode);
	
	if(noErr != result) {
		ERR("AUGraphAddNode failed: %i", result);
		return false;
	}
	
	AudioUnit effectUnit = NULL;
	result = AUGraphNodeInfo(mAUGraph, 
							 effectNode, 
							 NULL, 
							 &effectUnit);

	if(noErr != result) {
		ERR("AUGraphAddNode failed: %i", result);
		
		result = AUGraphRemoveNode(mAUGraph, effectNode);
		
		if(noErr != result)
			ERR("AUGraphRemoveNode failed: %i", result);

		return false;
	}
	
	result = AudioUnitSetProperty(effectUnit,
								  kAudioUnitProperty_StreamFormat, 
								  kAudioUnitScope_Input, 
								  0,
								  &mAUGraphFormat,
								  sizeof(mAUGraphFormat));

	if(noErr != result) {
		ERR("AudioUnitSetProperty(kAudioUnitProperty_StreamFormat) failed: %i", result);

		// If the property couldn't be set (the AU may not support this format), remove the new node
		result = AUGraphRemoveNode(mAUGraph, effectNode);

		if(noErr != result)
			ERR("AUGraphRemoveNode failed: %i", result);
				
		return false;
	}
	
	result = AudioUnitSetProperty(effectUnit,
								  kAudioUnitProperty_StreamFormat, 
								  kAudioUnitScope_Output, 
								  0,
								  &mAUGraphFormat,
								  sizeof(mAUGraphFormat));
	
	if(noErr != result) {
		ERR("AudioUnitSetProperty(kAudioUnitProperty_StreamFormat) failed: %i", result);
		
		// If the property couldn't be set (the AU may not support this format), remove the new node
		result = AUGraphRemoveNode(mAUGraph, effectNode);
		
		if(noErr != result)
			ERR("AUGraphRemoveNode failed: %i", result);
		
		return false;
	}

	// Insert the effect at the end of the graph, before the output node
	result = AUGraphDisconnectNodeInput(mAUGraph, 
										mOutputNode,
										0);

	if(noErr != result) {
		ERR("AUGraphDisconnectNodeInput failed: %i", result);
		
		result = AUGraphRemoveNode(mAUGraph, effectNode);
		
		if(noErr != result)
			ERR("AUGraphRemoveNode failed: %i", result);
		
		return false;
	}
	
	// Reconnect the nodes
	result = AUGraphConnectNodeInput(mAUGraph, 
									 sourceNode,
									 0,
									 effectNode,
									 0);
	if(noErr != result) {
		ERR("AUGraphConnectNodeInput failed: %i", result);
		return false;
	}
	
	result = AUGraphConnectNodeInput(mAUGraph, 
									 effectNode,
									 0,
									 mOutputNode,
									 0);
	if(noErr != result) {
		ERR("AUGraphConnectNodeInput failed: %i", result);
		return false;
	}
	
	result = AUGraphUpdate(mAUGraph, NULL);
	if(noErr != result) {
		ERR("AUGraphUpdate failed: %i", result);

		// If the update failed, restore the previous node state
		result = AUGraphConnectNodeInput(mAUGraph,
										 sourceNode,
										 0,
										 mOutputNode,
										 0);

		if(noErr != result) {
			ERR("AUGraphConnectNodeInput failed: %i", result);
			return false;
		}
	}
	
	if(NULL != effectUnit1)
		*effectUnit1 = effectUnit;
	
	return true;
}

bool AudioPlayer::RemoveEffect(AudioUnit effectUnit)
{
	assert(NULL != effectUnit);
	
	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	
	if(noErr != result) {
		ERR("AUGraphGetNodeCount failed: %i", result);
		return false;
	}
	
	AUNode effectNode = -1;
	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = -1;
		result = AUGraphGetIndNode(mAUGraph, 
								   nodeIndex, 
								   &node);
		
		if(noErr != result) {
			ERR("AUGraphGetIndNode failed: %i", result);
			return false;
		}
		
		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, 
								 node, 
								 NULL, 
								 &au);
		
		if(noErr != result) {
			ERR("AUGraphNodeInfo failed: %i", result);
			return false;
		}
		
		// This is the unit to remove
		if(effectUnit == au) {
			effectNode = node;
			break;
		}
	}
	
	if(-1 == effectNode) {
		ERR("Unable to find the AUNode for the specified AudioUnit");
		return false;
	}
	
	// Get the current input and output nodes for the node to delete
	UInt32 numInteractions = 0;
	result = AUGraphCountNodeInteractions(mAUGraph, 
										  effectNode, 
										  &numInteractions);
	if(noErr != result) {
		ERR("AUGraphCountNodeConnections failed: %i", result);
		return false;
	}
	
	AUNodeInteraction *interactions = static_cast<AUNodeInteraction *>(calloc(numInteractions, sizeof(AUNodeInteraction)));
	if(NULL == interactions) {
		ERR("Unable to allocate memory");
		return false;
	}

	result = AUGraphGetNodeInteractions(mAUGraph, 
										effectNode,
										&numInteractions, 
										interactions);
	
	if(noErr != result) {
		ERR("AUGraphGetNodeInteractions failed: %i", result);
		
		free(interactions), interactions = NULL;
		
		return false;
	}
	
	AUNode sourceNode = -1, destNode = -1;
	for(UInt32 interactionIndex = 0; interactionIndex < numInteractions; ++interactionIndex) {
		AUNodeInteraction interaction = interactions[interactionIndex];
		
		if(kAUNodeInteraction_Connection == interaction.nodeInteractionType) {
			if(effectNode == interaction.nodeInteraction.connection.destNode)
				sourceNode = interaction.nodeInteraction.connection.sourceNode;
			else if(effectNode == interaction.nodeInteraction.connection.sourceNode)
				destNode = interaction.nodeInteraction.connection.destNode;
		}
	}						
	
	free(interactions), interactions = NULL;
	
	if(-1 == sourceNode || -1 == destNode) {
		ERR("Unable to find the source or destination nodes");
		return false;
	}
	
	result = AUGraphDisconnectNodeInput(mAUGraph, effectNode, 0);
	if(noErr != result) {
		ERR("AUGraphDisconnectNodeInput failed: %i", result);
		return false;
	}
	
	result = AUGraphDisconnectNodeInput(mAUGraph, destNode, 0);
	if(noErr != result) {
		ERR("AUGraphDisconnectNodeInput failed: %i", result);
		return false;
	}
	
	result = AUGraphRemoveNode(mAUGraph, effectNode);
	if(noErr != result) {
		ERR("AUGraphRemoveNode failed: %i", result);
		return false;
	}
	
	// Reconnect the nodes
	result = AUGraphConnectNodeInput(mAUGraph, sourceNode, 0, destNode, 0);
	if(noErr != result) {
		ERR("AUGraphConnectNodeInput failed: %i", result);
		return false;
	}
	
	result = AUGraphUpdate(mAUGraph, NULL);
	if(noErr != result) {
		ERR("AUGraphUpdate failed: %i", result);
		return false;
	}
	
	return true;
}


#pragma mark Device Management


CFStringRef AudioPlayer::CreateOutputDeviceUID()
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mOutputNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return NULL;
	}

	AudioDeviceID deviceID = 0;
	UInt32 dataSize = sizeof(deviceID);

	result = AudioUnitGetProperty(au,
								  kAudioOutputUnitProperty_CurrentDevice,
								  kAudioUnitScope_Global,
								  0,
								  &deviceID,
								  &dataSize);
		
	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		return NULL;
	}
	
	CFStringRef deviceUID = NULL;
	dataSize = sizeof(deviceUID);
	result = AudioDeviceGetProperty(deviceID, 
									0, 
									FALSE, 
									kAudioDevicePropertyDeviceUID, 
									&dataSize, 
									&deviceUID);
	
	if(noErr != result) {
		ERR("AudioDeviceGetProperty (kAudioDevicePropertyDeviceUID) failed: %i", result);
		return NULL;
	}
	
	return deviceUID;
}

bool AudioPlayer::SetOutputDeviceUID(CFStringRef deviceUID)
{
	assert(NULL != deviceUID);
	
	AudioDeviceID		deviceID		= kAudioDeviceUnknown;
	UInt32				specifierSize	= 0;
	OSStatus			result			= noErr;
	
	if(NULL == deviceUID) {
		specifierSize = sizeof(deviceID);
		
		result = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, 
										  &specifierSize, 
										  &deviceID);
		
		if(noErr != result)
			ERR("AudioHardwareGetProperty (kAudioHardwarePropertyDefaultOutputDevice) failed: %i", result);
	}
	else {
		AudioValueTranslation translation;
		
		translation.mInputData			= &deviceUID;
		translation.mInputDataSize		= sizeof(deviceUID);
		translation.mOutputData			= &deviceID;
		translation.mOutputDataSize		= sizeof(deviceID);
		
		specifierSize					= sizeof(translation);
		
		result = AudioHardwareGetProperty(kAudioHardwarePropertyDeviceForUID, 
										  &specifierSize, 
										  &translation);
		
		if(noErr != result)
			ERR("AudioHardwareGetProperty (kAudioHardwarePropertyDeviceForUID) failed: %i", result);
	}
	
	if(noErr == result && kAudioDeviceUnknown != deviceID) {

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, 
								 mOutputNode, 
								 NULL, 
								 &au);
		
		// Update our output AU to use the currently selected device
		if(noErr == result) {
			result = AudioUnitSetProperty(au,
										  kAudioOutputUnitProperty_CurrentDevice,
										  kAudioUnitScope_Global,
										  0,
										  &deviceID,
										  sizeof(deviceID));
			
			if(noErr != result)
				ERR("AudioUnitSetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		}
		else
			ERR("AUGraphNodeInfo failed: %i", result);
	}

	return (noErr == result);
}

Float64 AudioPlayer::GetOutputDeviceSampleRate()
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mOutputNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return -1;
	}
	
	AudioDeviceID deviceID = 0;
	UInt32 dataSize = sizeof(deviceID);
	
	result = AudioUnitGetProperty(au,
								  kAudioOutputUnitProperty_CurrentDevice,
								  kAudioUnitScope_Global,
								  0,
								  &deviceID,
								  &dataSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		return -1;
	}
	
	Float64 sampleRate = -1;
	dataSize = sizeof(sampleRate);
	
	result = AudioDeviceGetProperty(deviceID, 
									0,
									FALSE, 
									kAudioDevicePropertyNominalSampleRate,
									&dataSize,
									&sampleRate);

	if(noErr != result) {
		ERR("AudioDeviceGetProperty (kAudioDevicePropertyNominalSampleRate) failed: %i", result);
		return -1;
	}
	
	return sampleRate;
}

bool AudioPlayer::SetOutputDeviceSampleRate(Float64 sampleRate)
{
	assert(0 < sampleRate);
	
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mOutputNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	AudioDeviceID deviceID = 0;
	UInt32 dataSize = sizeof(deviceID);
	
	result = AudioUnitGetProperty(au,
								  kAudioOutputUnitProperty_CurrentDevice,
								  kAudioUnitScope_Global,
								  0,
								  &deviceID,
								  &dataSize);

	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		return false;
	}
	
	// Determine if this will actually be a change
	Float64 currentSampleRate;
	dataSize = sizeof(currentSampleRate);
	result = AudioDeviceGetProperty(deviceID,
									0,
									FALSE,
									kAudioDevicePropertyNominalSampleRate,
									&dataSize,
									&currentSampleRate);

	if(noErr != result) {
		ERR("AudioDeviceGetProperty (kAudioDevicePropertyNominalSampleRate) failed: %i", result);
		return false;
	}
	
	// Nothing to do
	if(currentSampleRate == sampleRate)
		return true;
	
	// Set the sample rate
	result = AudioDeviceSetProperty(deviceID,
									NULL,
									0,
									FALSE,
									kAudioDevicePropertyNominalSampleRate,
									sizeof(sampleRate),
									&sampleRate);
	
	if(kAudioHardwareNoError != result)
		ERR("AudioDeviceSetProperty (kAudioDevicePropertyNominalSampleRate) failed: %i", result);

	return (noErr == result);
}

bool AudioPlayer::OutputDeviceIsHogged()
{
	// Get the output node's AudioUnit
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mOutputNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	// Get the current output device
	AudioDeviceID deviceID = kAudioDeviceUnknown;
	
	UInt32 specifierSize = sizeof(deviceID);
	result = AudioUnitGetProperty(au,
								  kAudioOutputUnitProperty_CurrentDevice,
								  kAudioUnitScope_Global,
								  0,
								  &deviceID,
								  &specifierSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty(kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		return false;
	}
	
	// Is it hogged by us?
	pid_t hogPID = static_cast<pid_t>(-1);
	specifierSize = sizeof(hogPID);
	result = AudioDeviceGetProperty(deviceID, 
									0,
									FALSE,
									kAudioDevicePropertyHogMode,
									&specifierSize,
									&hogPID);
	
	if(kAudioHardwareNoError != result) {
		ERR("AudioDeviceGetProperty(kAudioDevicePropertyHogMode) failed: %i", result);
		return false;
	}
	
	return (hogPID == getpid() ? true : false);
}

bool AudioPlayer::StartHoggingOutputDevice()
{
	// Get the output node's AudioUnit
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mOutputNode, 
									  NULL, 
									  &au);

	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	// Get the current output device
	AudioDeviceID deviceID = kAudioDeviceUnknown;
	
	UInt32 specifierSize = sizeof(deviceID);
	result = AudioUnitGetProperty(au,
								  kAudioOutputUnitProperty_CurrentDevice,
								  kAudioUnitScope_Global,
								  0,
								  &deviceID,
								  &specifierSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty(kAudioOutputUnitProperty_CurrentDevice) failed: %i", result);
		return false;
	}
	
	// Is it hogged already?
	pid_t hogPID = static_cast<pid_t>(-1);
	specifierSize = sizeof(hogPID);
	result = AudioDeviceGetProperty(deviceID, 
									0,
									FALSE, 
									kAudioDevicePropertyHogMode,
									&specifierSize,
									&hogPID);
	
	if(kAudioHardwareNoError != result) {
		ERR("AudioDeviceGetProperty(kAudioDevicePropertyHogMode) failed: %i", result);
		return false;
	}
	
	// The device isn't hogged, so attempt to hog it
	if(hogPID == static_cast<pid_t>(-1)) {
		hogPID = getpid();
		result = AudioDeviceSetProperty(deviceID,
										NULL, 
										0, FALSE,
										kAudioDevicePropertyHogMode,
										sizeof(hogPID),
										&hogPID);
		
		if(kAudioHardwareNoError != result) {
			ERR("AudioDeviceSetProperty(kAudioDevicePropertyHogMode) failed: %i", result);
			return false;
		}
	}
	else
		LOG("Device is already hogged by pid: %d", hogPID);
		
	return true;
}


#pragma mark Playlist Management


bool AudioPlayer::Play(CFURLRef url)
{
	assert(NULL != url);
	
	AudioDecoder *decoder = AudioDecoder::CreateDecoderForURL(url);
	
	if(NULL == decoder)
		return false;
	
	bool success = Play(decoder);
	
	if(false == success)
		delete decoder;
	
	return success;
}

bool AudioPlayer::Play(AudioDecoder *decoder)
{
	assert(NULL != decoder);
	
	bool wasPlaying = IsPlaying();
	if(true == wasPlaying)
		Pause();

	StopActiveDecoders();
	ResetAUGraph();
	
	mFramesDecoded = 0;
	mFramesRendered = 0;
	mNextDecoderStartingTimeStamp = 0;

	int lockResult = pthread_mutex_lock(&mMutex);
	
	if(0 != lockResult) {
		ERR("pthread_mutex_lock failed: %i", lockResult);
		return false;
	}
	
	mDecoderQueue.push_front(decoder);
	
	lockResult = pthread_mutex_unlock(&mMutex);

	if(0 != lockResult)
		ERR("pthread_mutex_unlock failed: %i", lockResult);
	
	OSStatus result = SetAUGraphFormat(decoder->GetFormat());

	if(noErr != result) {
		ERR("SetAUGraphFormat failed: %i", result);
		return false;
	}

	result = SetAUGraphChannelLayout(decoder->GetChannelLayout());
	
	if(noErr != result) {
		ERR("SetAUGraphChannelLayout failed: %i", result);
		return false;
	}
	
	// Allocate enough space in the ring buffer for the new format
	mRingBuffer->Allocate(decoder->GetFormat().mChannelsPerFrame,
						  decoder->GetFormat().mBytesPerFrame,
						  RING_BUFFER_SIZE_FRAMES);

	// Launch the reader thread for this decoder
	pthread_t fileReaderThread;							
	int creationResult = pthread_create(&fileReaderThread, NULL, fileReaderEntry, this);
	
	if(0 != creationResult) {
		ERR("pthread_create failed: %i", creationResult);
		return false;
	}
	
	if(true == wasPlaying)
		Play();

	return true;
}

bool AudioPlayer::Enqueue(CFURLRef url)
{
	assert(NULL != url);
	
	AudioDecoder *decoder = AudioDecoder::CreateDecoderForURL(url);
	
	if(NULL == decoder)
		return false;
	
	bool success = Enqueue(decoder);
	
	if(false == success)
		delete decoder;
	
	return success;
}

bool AudioPlayer::Enqueue(AudioDecoder *decoder)
{
	assert(NULL != decoder);
	
	// If there are no actve decoders and none in the queue, start this decoder immediately
	int lockResult = pthread_mutex_lock(&mMutex);
	
	if(0 != lockResult) {
		ERR("pthread_mutex_lock failed: %i", lockResult);
		return false;
	}
	
	std::deque<AudioDecoder *>::size_type decoderQueueSize = mDecoderQueue.size();
		
	lockResult = pthread_mutex_unlock(&mMutex);
		
	if(0 != lockResult)
		ERR("pthread_mutex_unlock failed: %i", lockResult);
	
	if(NULL == GetCurrentDecoderState() && 0 == decoderQueueSize)
		return Play(decoder);
	
	// Otherwise, enqueue this decoder if the format matches
	AudioUnit au = NULL;
	OSStatus auResult = AUGraphNodeInfo(mAUGraph, 
										mOutputNode, 
										NULL, 
										&au);
	
	if(noErr != auResult) {
		ERR("AUGraphNodeInfo failed: %i", auResult);
		return false;
	}
	
	AudioStreamBasicDescription format;
	UInt32 dataSize = sizeof(format);
	ComponentResult result = AudioUnitGetProperty(au,
												  kAudioUnitProperty_StreamFormat,
												  kAudioUnitScope_Input,
												  0,
												  &format,
												  &dataSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioUnitProperty_StreamFormat) failed: %i", result);
		return false;
	}

/*	AudioChannelLayout channelLayout;
	dataSize = sizeof(channelLayout);
	result = AudioUnitGetProperty(au,
								  kAudioUnitProperty_AudioChannelLayout,
								  kAudioUnitScope_Input,
								  0,
								  &channelLayout,
								  &dataSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioUnitProperty_AudioChannelLayout) failed: %i", result);
		return false;
	}*/
	
	AudioStreamBasicDescription		nextFormat			= decoder->GetFormat();
//	AudioChannelLayout				nextChannelLayout	= decoder->GetChannelLayout();

	bool	formatsMatch			= (nextFormat.mSampleRate == format.mSampleRate && nextFormat.mChannelsPerFrame == format.mChannelsPerFrame);
//	bool	channelLayoutsMatch		= channelLayoutsAreEqual(&nextChannelLayout, &channelLayout);

	// The two files can be joined only if they have the same formats and channel layouts
	if(false == formatsMatch /*|| false == channelLayoutsMatch*/)
		return false;
	
	// Add the decoder to the queue
	lockResult = pthread_mutex_lock(&mMutex);
	
	if(0 != lockResult) {
		ERR("pthread_mutex_lock failed: %i", lockResult);
		return false;
	}
	
	mDecoderQueue.push_back(decoder);
	
	lockResult = pthread_mutex_unlock(&mMutex);
	
	if(0 != lockResult)
		ERR("pthread_mutex_unlock failed: %i", lockResult);
	
	return true;
}

bool AudioPlayer::ClearQueuedDecoders()
{
	int lockResult = pthread_mutex_lock(&mMutex);
	
	if(0 != lockResult) {
		ERR("pthread_mutex_lock failed: %i", lockResult);
		return false;
	}
	
	while(false == mDecoderQueue.empty()) {
		AudioDecoder *decoder = mDecoderQueue.front();
		mDecoderQueue.pop_front();
		delete decoder;
	}
	
	lockResult = pthread_mutex_unlock(&mMutex);
	
	if(0 != lockResult)
		ERR("pthread_mutex_unlock failed: %i", lockResult);
	
	return true;	
}


#pragma mark Callbacks


OSStatus AudioPlayer::Render(AudioUnitRenderActionFlags		*ioActionFlags,
							 const AudioTimeStamp			*inTimeStamp,
							 UInt32							inBusNumber,
							 UInt32							inNumberFrames,
							 AudioBufferList				*ioData)
{

#pragma unused(inTimeStamp)
#pragma unused(inBusNumber)
	
	assert(NULL != ioActionFlags);
	assert(NULL != ioData);

	// If the ring buffer doesn't contain any valid audio, skip some work
	UInt32 framesAvailableToRead = static_cast<UInt32>(mFramesDecoded - mFramesRendered);
	if(0 == framesAvailableToRead) {
		*ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
		
		size_t byteCountToZero = inNumberFrames * sizeof(float);
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex) {
			memset(ioData->mBuffers[bufferIndex].mData, 0, byteCountToZero);
			ioData->mBuffers[bufferIndex].mDataByteSize = static_cast<UInt32>(byteCountToZero);
		}
		
		return noErr;
	}

	// Restrict reads to valid decoded audio
	UInt32 framesToRead = std::min(framesAvailableToRead, inNumberFrames);
	CARingBufferError result = mRingBuffer->Fetch(ioData, framesToRead, mFramesRendered, false);
	if(kCARingBufferError_OK != result) {
		ERR("CARingBuffer::Fetch() failed: %d, requested %d frames from %ld", result, framesToRead, static_cast<long>(mFramesRendered));
		return ioErr;
	}

	mFramesRenderedLastPass = framesToRead;
	OSAtomicAdd64Barrier(framesToRead, &mFramesRendered);
	
	// If the ring buffer didn't contain as many frames as were requested, fill the remainder with silence
	if(framesToRead != inNumberFrames) {
		LOG("Ring buffer contained insufficient data: %d / %d", framesToRead, inNumberFrames);
		
		UInt32 framesOfSilence = inNumberFrames - framesToRead;
		size_t byteCountToZero = framesOfSilence * sizeof(float);
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex) {
			float *bufferAlias = static_cast<float *>(ioData->mBuffers[bufferIndex].mData);
			memset(bufferAlias + framesToRead, 0, byteCountToZero);
			ioData->mBuffers[bufferIndex].mDataByteSize += static_cast<UInt32>(byteCountToZero);
		}
	}
	
	// If there is adequate space in the ring buffer for another chunk, signal the reader thread
	UInt32 framesAvailableToWrite = static_cast<UInt32>(RING_BUFFER_SIZE_FRAMES - (mFramesDecoded - mFramesRendered));
	if(RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES <= framesAvailableToWrite)
		semaphore_signal(mDecoderSemaphore);
	
	return noErr;
}

OSStatus AudioPlayer::DidRender(AudioUnitRenderActionFlags		*ioActionFlags,
								const AudioTimeStamp			*inTimeStamp,
								UInt32							inBusNumber,
								UInt32							inNumberFrames,
								AudioBufferList					*ioData)
{

#pragma unused(inTimeStamp)
#pragma unused(inBusNumber)
#pragma unused(inNumberFrames)
#pragma unused(ioData)

	if(kAudioUnitRenderAction_PostRender & (*ioActionFlags)) {

		// There is nothing to do if no frames were rendered
		if(0 == mFramesRenderedLastPass)
			return noErr;
		
		// mFramesRenderedLastPass contains the number of valid frames that were rendered
		// However, these could have come from any number of decoders depending on the buffer sizes
		// So it is necessary to split them up here

		SInt64 framesRemainingToDistribute = mFramesRenderedLastPass;

		for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
			DecoderStateData *decoderState = mActiveDecoders[i];
			
			if(NULL == decoderState)
				continue;
			
			if(decoderState->mReadyForCollection)
				continue;

			SInt64 decoderFramesRemaining = decoderState->mTotalFrames - decoderState->mFramesRendered;
			SInt64 framesFromThisDecoder = std::min(decoderFramesRemaining, static_cast<SInt64>(mFramesRenderedLastPass));
			
			if(0 == decoderState->mFramesRendered)
				decoderState->mDecoder->PerformRenderingStartedCallback();
			
			OSAtomicAdd64Barrier(framesFromThisDecoder, &decoderState->mFramesRendered);
			
			if(decoderState->mFramesRendered == decoderState->mTotalFrames) {
				decoderState->mDecoder->PerformRenderingFinishedCallback();
				
				// Since rendering is finished, signal the collector to clean up this decoder
				decoderState->mReadyForCollection = true;
				semaphore_signal(mCollectorSemaphore);
			}
			
			framesRemainingToDistribute -= framesFromThisDecoder;
			
			if(0 == framesRemainingToDistribute)
				break;
		}

		// If there are no more active decoders, stop playback
		if(NULL == GetCurrentDecoderState())
			Stop();
	}
	
	return noErr;
}

void * AudioPlayer::FileReaderThreadEntry()
{
	// ========================================
	// Make ourselves a high priority thread
	if(false == setThreadPolicy(FEEDER_THREAD_IMPORTANCE))
		ERR("Couldn't set feeder thread importance");

	// ========================================
	// Lock the queue and remove the head element, which contains the next decoder to use
	int lockResult = pthread_mutex_lock(&mMutex);
	
	if(0 != lockResult) {
		ERR("pthread_mutex_lock failed: %i", lockResult);
		
		// Stop now, to avoid risking data corruption
		return NULL;
	}
	
	AudioDecoder *decoder = mDecoderQueue.front();
	mDecoderQueue.pop_front();
	
	lockResult = pthread_mutex_unlock(&mMutex);
	
	if(0 != lockResult)
		ERR("pthread_mutex_unlock failed: %i", lockResult);

	// This only happens in rare cases when the user calls Enqueue() and then (immediately)
	// deletes the player before calling Play(). In this scenario it is possible for the
	// player's destructor to run before this function is called. When this happens the
	// destructor deletes all enqueued decoders and this function is left with nothing to work with.
	if(NULL == decoder) {
		ERR("FileReaderThreadEntry called with no decoders in queue");
		return NULL;
	}
	
	// ========================================
	// Create the decoder state and append to the list of active decoders
	DecoderStateData *decoderStateData = new DecoderStateData(decoder);
	decoderStateData->mDecodingThread = pthread_self();
	decoderStateData->mTimeStamp = mNextDecoderStartingTimeStamp;
	
	for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
		if(NULL != mActiveDecoders[i])
			continue;

		if(true == OSAtomicCompareAndSwapPtrBarrier(NULL, decoderStateData, reinterpret_cast<void **>(&mActiveDecoders[i])))
			break;
		else
			ERR("OSAtomicCompareAndSwapPtrBarrier failed");
	}
	
	SInt64 startTime = decoderStateData->mTimeStamp;

	// ========================================
	// Allocate the buffer list which will serve as the transport between the decoder and the ring buffer
	AudioStreamBasicDescription formatDescription = decoder->GetFormat();
	AudioBufferList *bufferList = static_cast<AudioBufferList *>(calloc(1, sizeof(AudioBufferList) + (sizeof(AudioBuffer) * (formatDescription.mChannelsPerFrame - 1))));
	
	bufferList->mNumberBuffers = formatDescription.mChannelsPerFrame;
	
	for(UInt32 i = 0; i < bufferList->mNumberBuffers; ++i) {
		bufferList->mBuffers[i].mData = static_cast<void *>(calloc(RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES, sizeof(float)));
		bufferList->mBuffers[i].mDataByteSize = RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES * sizeof(float);
		bufferList->mBuffers[i].mNumberChannels = 1;
	}

	// Two seconds and zero nanoseconds
	mach_timespec_t timeout = { 2, 0 };

	// ========================================
	// Decode the audio file in the ring buffer until finished or cancelled
	while(true == decoderStateData->mKeepDecoding) {
		
		// Fill the ring buffer with as much data as possible
		for(;;) {
			
			// Determine how many frames are available in the ring buffer
			UInt32 framesAvailableToWrite = static_cast<UInt32>(RING_BUFFER_SIZE_FRAMES - (mFramesDecoded - mFramesRendered));
			
			// Force writes to the ring buffer to be at least RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES
			if(framesAvailableToWrite >= RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES) {

				// Seek to the specified frame
				if(-1 != decoderStateData->mFrameToSeek) {
					SInt64 currentFrameBeforeSeeking = decoder->GetCurrentFrame();

					SInt64 newFrame = decoder->SeekToFrame(decoderStateData->mFrameToSeek);
					
					if(newFrame != decoderStateData->mFrameToSeek)
						ERR("Error seeking to frame %ld", static_cast<long>(decoderStateData->mFrameToSeek));

					// Update the seek request
					if(false == OSAtomicCompareAndSwap64Barrier(decoderStateData->mFrameToSeek, -1, &decoderStateData->mFrameToSeek))
						ERR("OSAtomicCompareAndSwap64Barrier failed");
					
					// If the seek failed do not update the counters
					if(-1 != newFrame) {
						SInt64 framesSkipped = newFrame - currentFrameBeforeSeeking;
						
						// Treat the skipped frames as if they were rendered, and update the counters accordingly
						if(false == OSAtomicCompareAndSwap64Barrier(decoderStateData->mFramesRendered, newFrame, &decoderStateData->mFramesRendered))
							ERR("OSAtomicCompareAndSwap64Barrier failed");
						
						OSAtomicAdd64Barrier(framesSkipped, &mFramesDecoded);
						if(false == OSAtomicCompareAndSwap64Barrier(mFramesRendered, mFramesDecoded, &mFramesRendered))
							ERR("OSAtomicCompareAndSwap64Barrier failed");
						
						ResetAUGraph();
					}
				}
				
				SInt64 startingFrameNumber = decoder->GetCurrentFrame();
				
				// Read the input chunk
				UInt32 framesDecoded = decoder->ReadAudio(bufferList, RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES);

				// If this is the first frame, decoding is just starting
				if(0 == startingFrameNumber)
					decoder->PerformDecodingStartedCallback();
								
				// Store the decoded audio
				if(0 != framesDecoded) {
					// Copy the decoded audio to the ring buffer
					CARingBufferError result = mRingBuffer->Store(bufferList, framesDecoded, startingFrameNumber + startTime);
					if(kCARingBufferError_OK != result)
						ERR("CARingBuffer::Store() failed: %i", result);
					
					OSAtomicAdd64Barrier(framesDecoded, &mFramesDecoded);
				}
				
				// If no frames were returned, this is the end of stream
				if(0 == framesDecoded) {
					decoder->PerformDecodingFinishedCallback();
					
					// This thread is complete					
					decoderStateData->mDecodingThread = static_cast<pthread_t>(0);
					decoderStateData->mKeepDecoding = false;
					
					// Some formats (MP3) may not know the exact number of frames in advance
					// without processing the entire file, which is a potentially slow operation
					// Rather than require preprocessing to ensure an accurate frame count, update 
					// it here so EOS is correctly detected in DidRender()
					decoderStateData->mTotalFrames = startingFrameNumber;
					
					OSAtomicAdd64Barrier(startingFrameNumber, &mNextDecoderStartingTimeStamp);
					
					// Lock the queue and determine if there is another decoder which should be started
					lockResult = pthread_mutex_lock(&mMutex);
					
					if(0 == lockResult) {
						std::deque<AudioDecoder *>::size_type decoderQueueSize = mDecoderQueue.size();
						
						lockResult = pthread_mutex_unlock(&mMutex);
						
						if(0 != lockResult)
							ERR("pthread_mutex_unlock failed: %i", lockResult);
						
						// Launch another reader thread if another decoder is present in the queue
						if(0 < decoderQueueSize) {
							pthread_t fileReaderThread;							
							int creationResult = pthread_create(&fileReaderThread, NULL, fileReaderEntry, this);
							
							if(0 != creationResult)
								ERR("pthread_create failed: %i", creationResult);
						}
					}
					else
						ERR("pthread_mutex_lock failed: %i", lockResult);
					
					break;
				}
			}
			// Not enough space remains in the ring buffer to write an entire decoded chunk
			else
				break;
		}
		
		// Wait for the audio rendering thread to signal us that it could use more data, or for the timeout to happen
		semaphore_timedwait(mDecoderSemaphore, timeout);
	}
	
	// ========================================
	// Clean up
	if(bufferList) {
		for(UInt32 bufferIndex = 0; bufferIndex < bufferList->mNumberBuffers; ++bufferIndex)
			free(bufferList->mBuffers[bufferIndex].mData), bufferList->mBuffers[bufferIndex].mData = NULL;
		
		free(bufferList), bufferList = NULL;
	}

	return NULL;
}

void * AudioPlayer::CollectorThreadEntry()
{
	// Two seconds and zero nanoseconds
	mach_timespec_t timeout = { 2, 0 };

	while(mKeepCollecting) {
		
		for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
			DecoderStateData *decoderState = mActiveDecoders[i];
			
			if(NULL == decoderState)
				continue;
			
			if(false == decoderState->mReadyForCollection)
				continue;
			
			bool swapSucceeded = OSAtomicCompareAndSwapPtrBarrier(decoderState, NULL, reinterpret_cast<void **>(&mActiveDecoders[i]));
			
			if(swapSucceeded)
				delete decoderState, decoderState = NULL;
		}
		
		// Wait for any thread to signal us to try and collect finished decoders
		semaphore_timedwait(mCollectorSemaphore, timeout);
	}
	
	return NULL;
}


#pragma mark AUGraph Utilities


OSStatus AudioPlayer::CreateAUGraph()
{
	OSStatus result = NewAUGraph(&mAUGraph);

	if(noErr != result) {
		ERR("NewAUGraph failed: %i", result);
		return result;
	}
	
	// The graph will look like:
	// Peak Limiter -> Effects -> Output
	ComponentDescription desc;

	// Set up the peak limiter node
	desc.componentType			= kAudioUnitType_Effect;
	desc.componentSubType		= kAudioUnitSubType_PeakLimiter;
	desc.componentManufacturer	= kAudioUnitManufacturer_Apple;
	desc.componentFlags			= 0;
	desc.componentFlagsMask		= 0;
	
	result = AUGraphAddNode(mAUGraph, &desc, &mLimiterNode);

	if(noErr != result) {
		ERR("AUGraphAddNode failed: %i", result);
		return result;
	}
	
	// Set up the output node
	desc.componentType			= kAudioUnitType_Output;
	desc.componentSubType		= kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer	= kAudioUnitManufacturer_Apple;
	desc.componentFlags			= 0;
	desc.componentFlagsMask		= 0;
	
	result = AUGraphAddNode(mAUGraph, &desc, &mOutputNode);

	if(noErr != result) {
		ERR("AUGraphAddNode failed: %i", result);
		return result;
	}
	
	result = AUGraphConnectNodeInput(mAUGraph, mLimiterNode, 0, mOutputNode, 0);

	if(noErr != result) {
		ERR("AUGraphConnectNodeInput failed: %i", result);
		return result;
	}
	
	// Install the input callback
	AURenderCallbackStruct cbs = { myAURenderCallback, this };
	result = AUGraphSetNodeInputCallback(mAUGraph, mLimiterNode, 0, &cbs);

	if(noErr != result) {
		ERR("AUGraphSetNodeInputCallback failed: %i", result);
		return result;
	}
	
	// Open the graph
	result = AUGraphOpen(mAUGraph);

	if(noErr != result) {
		ERR("AUGraphOpen failed: %i", result);
		return result;
	}
	
	// Initialize the graph
	result = AUGraphInitialize(mAUGraph);

	if(noErr != result) {
		ERR("AUGraphInitialize failed: %i", result);
		return result;
	}
	
	// Install the render notification
	result = AUGraphAddRenderNotify(mAUGraph, auGraphDidRender, this);

	if(noErr != result) {
		ERR("AUGraphAddRenderNotify failed: %i", result);
		return result;
	}
	
	return noErr;
}

OSStatus AudioPlayer::DisposeAUGraph()
{
	Boolean graphIsRunning = FALSE;
	OSStatus result = AUGraphIsRunning(mAUGraph, &graphIsRunning);

	if(noErr != result) {
		ERR("AUGraphIsRunning failed: %i", result);
		return result;
	}
	
	if(graphIsRunning) {
		result = AUGraphStop(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphStop failed: %i", result);
			return result;
		}
	}
	
	Boolean graphIsInitialized = FALSE;	
	result = AUGraphIsInitialized(mAUGraph, &graphIsInitialized);

	if(noErr != result) {
		ERR("AUGraphIsInitialized failed: %i", result);
		return result;
	}
	
	if(graphIsInitialized) {
		result = AUGraphUninitialize(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphUninitialize failed: %i", result);
			return result;
		}
	}
	
	result = AUGraphClose(mAUGraph);

	if(noErr != result) {
		ERR("AUGraphClose failed: %i", result);
		return result;
	}
	
	result = ::DisposeAUGraph(mAUGraph);

	if(noErr != result) {
		ERR("DisposeAUGraph failed: %i", result);
		return result;
	}
	
	mAUGraph = NULL;
	
	return noErr;
}

OSStatus AudioPlayer::ResetAUGraph()
{
	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		ERR("AUGraphGetNodeCount failed: %i", result);
		return result;
	}
	
	for(UInt32 i = 0; i < nodeCount; ++i) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, i, &node);
		if(noErr != result) {
			ERR("AUGraphGetIndNode failed: %i", result);
			return result;
		}
		
		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		if(noErr != result) {
			ERR("AUGraphNodeInfo failed: %i", result);
			return result;
		}
		
		result = AudioUnitReset(au, kAudioUnitScope_Global, 0);
		if(noErr != result) {
			ERR("AudioUnitReset failed: %i", result);
			return result;
		}
	}
	
	return noErr;
}

Float64 AudioPlayer::GetAUGraphLatency()
{
	Float64 graphLatency = 0;
	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);

	if(noErr != result) {
		ERR("AUGraphGetNodeCount failed: %i", result);
		return -1;
	}
	
	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, nodeIndex, &node);

		if(noErr != result) {
			ERR("AUGraphGetIndNode failed: %i", result);
			return -1;
		}
		
		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);

		if(noErr != result) {
			ERR("AUGraphNodeInfo failed: %i", result);
			return -1;
		}
		
		Float64 latency = 0;
		UInt32 dataSize = sizeof(latency);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &dataSize);

		if(noErr != result) {
			ERR("AudioUnitGetProperty failed: %i", result);
			return -1;
		}
		
		graphLatency += latency;
	}
	
	return graphLatency;
}

Float64 AudioPlayer::GetAUGraphTailTime()
{
	Float64 graphTailTime = 0;
	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	
	if(noErr != result) {
		ERR("AUGraphGetNodeCount failed: %i", result);
		return -1;
	}
	
	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, nodeIndex, &node);
		
		if(noErr != result) {
			ERR("AUGraphGetIndNode failed: %i", result);
			return -1;
		}
		
		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		
		if(noErr != result) {
			ERR("AUGraphNodeInfo failed: %i", result);
			return -1;
		}
		
		Float64 tailTime = 0;
		UInt32 dataSize = sizeof(tailTime);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &tailTime, &dataSize);
		
		if(noErr != result) {
			ERR("AudioUnitGetProperty (kAudioUnitProperty_TailTime) failed: %i", result);
			return -1;
		}
		
		graphTailTime += tailTime;
	}
	
	return graphTailTime;
}

OSStatus AudioPlayer::SetPropertyOnAUGraphNodes(AudioUnitPropertyID propertyID, const void *propertyData, UInt32 propertyDataSize)
{
	assert(NULL != propertyData);
	assert(0 < propertyDataSize);
	
	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);

	if(noErr != result) {
		ERR("AUGraphGetNodeCount failed: %i", result);
		return result;
	}
	
	// Iterate through the nodes and attempt to set the property
	for(UInt32 i = 0; i < nodeCount; ++i) {
		AUNode node;
		result = AUGraphGetIndNode(mAUGraph, i, &node);

		if(noErr != result) {
			ERR("AUGraphGetIndNode failed: %i", result);
			return result;
		}
		
		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);

		if(noErr != result) {
			ERR("AUGraphNodeInfo failed: %i", result);
			return result;
		}
		
		if(mOutputNode == node) {
			// For AUHAL as the output node, you can't set the device side, so just set the client side
			result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Input, 0, propertyData, propertyDataSize);

			if(noErr != result) {
				ERR("AudioUnitSetProperty ('%.4s') failed: %i", reinterpret_cast<const char *>(&propertyID), result);
				return result;
			}
			
// IO must be enabled for this to work
/*			err = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Output, 1, propertyData, propertyDataSize);

			if(noErr != err)
				return err;*/
		}
		else {
			UInt32 elementCount = 0;
			UInt32 dataSize = sizeof(elementCount);
			result = AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &elementCount, &dataSize);

			if(noErr != result) {
				ERR("AudioUnitGetProperty (kAudioUnitProperty_ElementCount) failed: %i", result);
				return result;
			}
			
			for(UInt32 j = 0; j < elementCount; ++j) {
/*				Boolean writable;
				err = AudioUnitGetPropertyInfo(au, propertyID, kAudioUnitScope_Input, j, &dataSize, &writable);

				if(noErr != err && kAudioUnitErr_InvalidProperty != err)
					return err;
				 
				if(kAudioUnitErr_InvalidProperty == err || !writable)
					continue;*/
				
				result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Input, j, propertyData, propertyDataSize);

				if(noErr != result) {
					ERR("AudioUnitSetProperty ('%.4s') failed: %i", reinterpret_cast<const char *>(&propertyID), result);
					return result;
				}
			}
			
			elementCount = 0;
			dataSize = sizeof(elementCount);
			result = AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &elementCount, &dataSize);

			if(noErr != result) {
				ERR("AudioUnitGetProperty (kAudioUnitProperty_ElementCount) failed: %i", result);
				return result;
			}
			
			for(UInt32 j = 0; j < elementCount; ++j) {
/*				Boolean writable;
				err = AudioUnitGetPropertyInfo(au, propertyID, kAudioUnitScope_Output, j, &dataSize, &writable);

				if(noErr != err && kAudioUnitErr_InvalidProperty != err)
					return err;
				 
				if(kAudioUnitErr_InvalidProperty == err || !writable)
					continue;*/
				
				result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Output, j, propertyData, propertyDataSize);

				if(noErr != result) {
					ERR("AudioUnitSetProperty ('%.4s') failed: %i", reinterpret_cast<const char *>(&propertyID), result);
					return result;
				}
			}
		}
	}
	
	return noErr;
}

OSStatus AudioPlayer::SetAUGraphFormat(AudioStreamBasicDescription format)
{
	AUNodeInteraction *interactions = NULL;
	
	// ========================================
	// If the graph is running, stop it
	Boolean graphIsRunning = FALSE;
	OSStatus result = AUGraphIsRunning(mAUGraph, &graphIsRunning);

	if(noErr != result) {
		ERR("AUGraphIsRunning failed: %i", result);
		return result;
	}
	
	if(graphIsRunning) {
		result = AUGraphStop(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphStop failed: %i", result);
			return result;
		}
	}
	
	// ========================================
	// If the graph is initialized, uninitialize it
	Boolean graphIsInitialized = FALSE;
	result = AUGraphIsInitialized(mAUGraph, &graphIsInitialized);

	if(noErr != result) {
		ERR("AUGraphIsInitialized failed: %i", result);
		return result;
	}
	
	if(graphIsInitialized) {
		result = AUGraphUninitialize(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphUninitialize failed: %i", result);
			return result;
		}
	}
	
	// ========================================
	// Save the interaction information and then clear all the connections
	UInt32 interactionCount = 0;
	result = AUGraphGetNumberOfInteractions(mAUGraph, &interactionCount);

	if(noErr != result) {
		ERR("AUGraphGetNumberOfInteractions failed: %i", result);
		return result;
	}
	
	interactions = static_cast<AUNodeInteraction *>(calloc(interactionCount, sizeof(AUNodeInteraction)));
	if(NULL == interactions)
		return memFullErr;
	
	for(UInt32 i = 0; i < interactionCount; ++i) {
		result = AUGraphGetInteractionInfo(mAUGraph, i, &interactions[i]);

		if(noErr != result) {
			ERR("AUGraphGetInteractionInfo failed: %i", result);

			free(interactions);

			return result;
		}
	}
	
	result = AUGraphClearConnections(mAUGraph);

	if(noErr != result) {
		ERR("AUGraphClearConnections failed: %i", result);

		free(interactions);
		
		return result;
	}
	
	// ========================================
	// Attempt to set the new stream format
	result = SetPropertyOnAUGraphNodes(kAudioUnitProperty_StreamFormat, &format, sizeof(format));

	if(noErr != result) {
		
		// If the new format could not be set, restore the old format to ensure a working graph
		OSStatus newErr = SetPropertyOnAUGraphNodes(kAudioUnitProperty_StreamFormat, &mAUGraphFormat, sizeof(mAUGraphFormat));

		if(noErr != newErr)
			ERR("Unable to restore AUGraph format: %i", result);

		// Do not free connections here, so graph can be rebuilt
		result = newErr;
	}
	else
		mAUGraphFormat = format;

	
	// ========================================
	// Restore the graph's connections and input callbacks
	for(UInt32 i = 0; i < interactionCount; ++i) {
		switch(interactions[i].nodeInteractionType) {
				
			// Reestablish the connection
			case kAUNodeInteraction_Connection:
			{
				result = AUGraphConnectNodeInput(mAUGraph, 
												 interactions[i].nodeInteraction.connection.sourceNode, 
												 interactions[i].nodeInteraction.connection.sourceOutputNumber,
												 interactions[i].nodeInteraction.connection.destNode, 
												 interactions[i].nodeInteraction.connection.destInputNumber);

				if(noErr != result) {
					ERR("AUGraphConnectNodeInput failed: %i", result);

					free(interactions), interactions = NULL;
					
					return result;
				}
				
				break;
			}
				
			// Reestablish the input callback
			case kAUNodeInteraction_InputCallback:
			{
				result = AUGraphSetNodeInputCallback(mAUGraph, 
												  interactions[i].nodeInteraction.inputCallback.destNode, 
												  interactions[i].nodeInteraction.inputCallback.destInputNumber,
												  &interactions[i].nodeInteraction.inputCallback.cback);

				if(noErr != result) {
					ERR("AUGraphSetNodeInputCallback failed: %i", result);

					free(interactions), interactions = NULL;
					
					return result;
				}
				
				break;
			}				
		}
	}
	
	free(interactions), interactions = NULL;
	
	// ========================================
	// Output units perform sample rate conversion if the input sample rate is not equal to
	// the output sample rate. For high sample rates, the sample rate conversion can require 
	// more rendered frames than are available by default in kAudioUnitProperty_MaximumFramesPerSlice (512)
	// For example, 192 KHz audio converted to 44.1 HHz requires approximately (192 / 44.1) * 512 = 2229 frames
	// So if the input and output sample rates on the output device don't match, adjust 
	// kAudioUnitProperty_MaximumFramesPerSlice to ensure enough audio data is passed per render cycle

	AudioUnit au = NULL;
	result = AUGraphNodeInfo(mAUGraph, 
							 mOutputNode, 
							 NULL, 
							 &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return result;
	}

	Float64 inputSampleRate = 0;
	UInt32 dataSize = sizeof(inputSampleRate);
	result = AudioUnitGetProperty(au, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &inputSampleRate, &dataSize);

	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioUnitProperty_SampleRate) [kAudioUnitScope_Input] failed: %i", result);
		return result;
	}
	
	Float64 outputSampleRate = 0;
	dataSize = sizeof(outputSampleRate);
	result = AudioUnitGetProperty(au, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &outputSampleRate, &dataSize);

	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioUnitProperty_SampleRate) [kAudioUnitScope_Output] failed: %i", result);
		return result;
	}
	
	if(inputSampleRate != outputSampleRate) {
		LOG("Input sample rate (%f) and output sample rate (%f) don't match", inputSampleRate, outputSampleRate);

		UInt32 currentMaxFrames = 0;
		dataSize = sizeof(currentMaxFrames);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &currentMaxFrames, &dataSize);
		
		if(noErr != result) {
			ERR("AudioUnitGetProperty (kAudioUnitProperty_MaximumFramesPerSlice) failed: %i", result);
			return result;
		}
		
		Float64 ratio = inputSampleRate / outputSampleRate;
		Float64 multiplier = std::max(1.0, ceil(ratio));
		
		// Round up to the nearest power of 16
		UInt32 newMaxFrames = static_cast<UInt32>(currentMaxFrames * multiplier);
		newMaxFrames += 16;
		newMaxFrames &= 0xFFFFFFF0;

		if(newMaxFrames > currentMaxFrames) {
			LOG("Adjusting kAudioUnitProperty_MaximumFramesPerSlice to %d", newMaxFrames);
			
			result = SetPropertyOnAUGraphNodes(kAudioUnitProperty_MaximumFramesPerSlice, &newMaxFrames, sizeof(newMaxFrames));
			
			if(noErr != result) {
				ERR("SetPropertyOnAUGraphNodes (kAudioUnitProperty_MaximumFramesPerSlice) failed: %i", result);
				return result;
			}
		}
	}

	// If the graph was initialized, reinitialize it
	if(graphIsInitialized) {
		result = AUGraphInitialize(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphInitialize failed: %i", result);
			return result;
		}
	}
	
	// If the graph was running, restart it
	if(graphIsRunning) {
		result = AUGraphStart(mAUGraph);

		if(noErr != result) {
			ERR("AUGraphStart failed: %i", result);
			return result;
		}
	}
	
	return noErr;
}

OSStatus AudioPlayer::SetAUGraphChannelLayout(AudioChannelLayout /*channelLayout*/)
{
	 // Attempt to set the new channel layout
//	 OSStatus result = AudioUnitSetProperty(mOutputUnit,
//											kAudioUnitProperty_AudioChannelLayout, 
//											kAudioUnitScope_Input, 
//											0,
//											&channelLayout, 
//											sizeof(channelLayout));
//
//	 if(noErr != result) {
//		 // If the new format could not be set, restore the old format to ensure a working graph
//		 OSStatus newResult = SetPropertyOnAUGraphNodes(kAudioUnitProperty_AudioChannelLayout, 
//														&mChannelLayout, 
//														sizeof(mChannelLayout));
//		 
//		 
//		 OSStatus newErr = AudioUnitSetProperty(mOutputUnit, 
//												kAudioUnitProperty_AudioChannelLayout,
//												kAudioUnitScope_Input, 
//												0,
//												&channelLayout, 
//												sizeof(channelLayout));
//
//		 if(noErr != newResult)
//			 LOG("Unable to restore AUGraph channel layout: %i", newResult);
//	 
//		 return result;
//	 }

	return noErr;
}

bool AudioPlayer::EnablePreGain(UInt32 flag)
{
	if(flag && IsPreGainEnabled())
		return true;
	else if(!flag && false == IsPreGainEnabled())
		return true;
	
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mLimiterNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	result = AudioUnitSetProperty(au, 
								  kAudioUnitProperty_BypassEffect,
								  kAudioUnitScope_Global, 
								  0, 
								  &flag, 
								  sizeof(flag));
	
	if(noErr != result) {
		ERR("AudioUnitSetProperty (kAudioUnitProperty_BypassEffect) failed: %i", result);
		return false;
	}
	
	return true;
}

bool AudioPlayer::IsPreGainEnabled()
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, 
									  mLimiterNode, 
									  NULL, 
									  &au);
	
	if(noErr != result) {
		ERR("AUGraphNodeInfo failed: %i", result);
		return false;
	}
	
	UInt32 bypassed	= FALSE;
	UInt32 dataSize	= sizeof(bypassed);
	
	result = AudioUnitGetProperty(au, 
								  kAudioUnitProperty_BypassEffect, 
								  kAudioUnitScope_Global, 
								  0,
								  &bypassed,
								  &dataSize);
	
	if(noErr != result) {
		ERR("AudioUnitGetProperty (kAudioUnitProperty_BypassEffect) failed: %i", result);
		return false;
	}
	
	return bypassed;
}


#pragma mark Other Utilities


DecoderStateData * AudioPlayer::GetCurrentDecoderState()
{
	DecoderStateData *result = NULL;
	for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
		DecoderStateData *decoderState = mActiveDecoders[i];
		
		if(NULL == decoderState)
			continue;
		
		if(true == decoderState->mReadyForCollection)
			continue;
		
		if(NULL == result)
			result = decoderState;
		else if(decoderState->mTimeStamp < result->mTimeStamp)
			result = decoderState;
	}
	
	return result;
}

void AudioPlayer::StopActiveDecoders()
{
	// End any still-active decoders
	for(UInt32 i = 0; i < kActiveDecoderArraySize; ++i) {
		DecoderStateData *decoderState = mActiveDecoders[i];
		
		if(NULL == decoderState)
			continue;
		
		decoderState->mKeepDecoding = false;
		decoderState->mReadyForCollection = true;
	}
	
	// Signal the collector to collect 
	semaphore_signal(mDecoderSemaphore);
	semaphore_signal(mCollectorSemaphore);
}
