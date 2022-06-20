#pragma warning(push, 0)
#include <sstream>
#include <ios>
#pragma warning(pop)

#include "OneWireAnalyzer.h"
#include "OneWireAnalyzerSettings.h"  
#include <AnalyzerChannelData.h>


OneWireAnalyzer::OneWireAnalyzer() 
:	mSettings( new OneWireAnalyzerSettings() ),
	Analyzer2(),
	mSimulationInitilized( false )
{ 
	SetAnalyzerSettings( mSettings.get() );
}

OneWireAnalyzer::~OneWireAnalyzer()
{
	KillThread();
}

void OneWireAnalyzer::SetupResults()
{
	mResults.reset( new OneWireAnalyzerResults( this, mSettings.get() ) );
	SetAnalyzerResults( mResults.get() );
	mResults->AddChannelBubblesWillAppearOn( mSettings->mOneWireChannel );
}

void OneWireAnalyzer::WorkerThread()
{
	//mResults->AddChannelBubblesWillAppearOn( mSettings->mOneWireChannel );

	mOneWire = GetAnalyzerChannelData( mSettings->mOneWireChannel );

	U64 starting_sample  = mOneWire->GetSampleNumber();
	//one_wire.MoveToSample( starting_sample );

	U64 current_sample = starting_sample;

	mSampleRateHz = this->GetSampleRate();

	mCurrentState = UnknownState;
	bool force_overdrive = mSettings->mOverdrive;
	mOverdrive = force_overdrive;
	
	mRisingEdgeSample = current_sample;
	mFallingEdgeSample = 0;
	mByteStartSample = 0;
	mBlockPulseAdvance = false;

	for( ; ; )
	{
		if( mBlockPulseAdvance == false )
		{
			mPreviousRisingEdgeSample = mRisingEdgeSample;

			//one_wire.MoveRightUntilBitChanges( true, true );
			//most of the time, we will be entering on a falling edge.

			current_sample = mOneWire->GetSampleNumber();
			if( (mOneWire->GetBitState() == BIT_LOW) &&(current_sample > mFallingEdgeSample) )
			{
				//it is possible that we have advanced into a reset pulse.
				//mFallingEdgeSample will contain the falling edge that we came in on. we should be inside by either 3us or 30 us.
				
				U64 next_rising_edge_sample = mOneWire->GetSampleOfNextEdge();
				U64 low_pulse_length = next_rising_edge_sample - mFallingEdgeSample;

				//test for a reset condition.
				U64 minimum_pulse_width = UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE );
				U64 spec_pulse_width = UsToSamples( SPEC_RESET_PULSE );
				U64 maximum_pulse_width = mLowPulseLength + 1;
				if( force_overdrive || mOverdrive )
				{
					minimum_pulse_width = UsToSamples( SPEC_OVD_RESET_PULSE - MARGIN_INSIDE_OVD_RESET_PULSE );
					spec_pulse_width = UsToSamples( SPEC_OVD_RESET_PULSE );
					maximum_pulse_width = UsToSamples( SPEC_MAX_OVD_RESET_PULSE + MARGIN_OUTSIDE_OVD_RESET_PULSE );
				}
				if( ( mLowPulseLength > minimum_pulse_width ) && ( mLowPulseLength < maximum_pulse_width ) )
				{
					//Reset Pulse Detected.
					//Print Reset Bubble.
					bool flag_warning = false;
					if( force_overdrive || mOverdrive )
					{
						if( low_pulse_length > UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE ) )
						{ 
							if( low_pulse_length < UsToSamples( SPEC_RESET_PULSE ) )
								flag_warning = true;
						}
					}
					else
					{
						if( low_pulse_length < spec_pulse_width )
							flag_warning = true;
					}
					mResults->CommitPacketAndStartNewPacket();
					if( flag_warning == true )
						//TODO:  RecordBubble( one_wire_bubbles, mFallingEdgeSample, mRisingEdgeSample, RestartPulse, mLowPulseLength );
						RecordFrame(mFallingEdgeSample, next_rising_edge_sample, RestartPulse, 0, true );
					else
						//TODO:  RecordBubble( one_wire_bubbles, mFallingEdgeSample, mRisingEdgeSample, RestartPulse );
						RecordFrame(mFallingEdgeSample, next_rising_edge_sample, RestartPulse );

					if (low_pulse_length > UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE ))
						mOverdrive = false;

					mRomBitsRecieved = 0;
					mRomDetected = 0;
					mDataDetected = 0;
					mDataBitsRecieved = 0;

					mCurrentState = ResetDetectedState;
					mOneWire->AdvanceToNextEdge(); //advance to the rising edge out of the reset pulse, so we don't detect it twice.
					
					continue;
				}
			} 
			

			mOneWire->AdvanceToNextEdge();
			
			//this happens every time a zero is recorded.
			if( mOneWire->GetBitState() != BIT_LOW )
			{
				mRisingEdgeSample = mOneWire->GetSampleNumber();
				mOneWire->AdvanceToNextEdge();
			}

			mFallingEdgeSample = mOneWire->GetSampleNumber();//one_wire.GetSampleNumber();

			mHighPulseLength = mFallingEdgeSample - mRisingEdgeSample;

			//one_wire.MoveRightUntilBitChanges( true, true );
			mRisingEdgeSample = mOneWire->GetSampleOfNextEdge();
			//mOneWire->AdvanceToNextEdge();

			//mRisingEdgeSample = mOneWire->GetSampleNumber();//one_wire.GetSampleNumber();
			mLowPulseLength = mRisingEdgeSample - mFallingEdgeSample;
		
			mLowPulseTime = SamplesToUs( mLowPulseLength ); //micro seconds
			mHighPulseTime = SamplesToUs( mHighPulseLength );
			if (mHighPulseLength > UsToSamples( 1000 ))
			{
				mCurrentState = NewPacketState;
				
				//RecordFrame(mFallingEdgeSample, mFallingEdgeSample + mHighPulseLength, RestartPulse, 0, true );
			}

			U64 min_high_pulse_samples = UsToSamples( 10 );
			U64 min_low_pulse_samples = UsToSamples( 10 );
			while(mHighPulseLength < min_high_pulse_samples)
			{
				mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::X, mSettings->mOneWireChannel );
				mOneWire->AdvanceToNextEdge();
				mOneWire->AdvanceToNextEdge();
				mFallingEdgeSample = mOneWire->GetSampleNumber();
				mHighPulseLength = mFallingEdgeSample - mRisingEdgeSample;
				mHighPulseTime = SamplesToUs( mHighPulseLength );
				mRisingEdgeSample = mOneWire->GetSampleOfNextEdge();

				mLowPulseLength = mRisingEdgeSample - mFallingEdgeSample;
				mLowPulseTime = SamplesToUs( mLowPulseLength ); //micro seconds
				//continue;
			}

			while( (mLowPulseLength < min_low_pulse_samples) )
			{
				mOneWire->AdvanceToNextEdge();
				mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::X, mSettings->mOneWireChannel );
				mOneWire->AdvanceToNextEdge(); //next neg edge.
				mFallingEdgeSample = mOneWire->GetSampleNumber();//one_wire.GetSampleNumber();
				mRisingEdgeSample = mOneWire->GetSampleOfNextEdge();
				mLowPulseLength = mRisingEdgeSample - mFallingEdgeSample;
				mLowPulseTime = SamplesToUs( mLowPulseLength ); //micro seconds
			}
			if (mCurrentState == NewPacketState)
			{
				mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::Start, mSettings->mOneWireChannel );
			}
			else
			{
				mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::DownArrow, mSettings->mOneWireChannel );
			}

			//At this point, we should be sitting on a negative edge, and have the pulse length of the low pulse in front, and the high pulse behind.
		}
		mBlockPulseAdvance = false;

		//Test for a reset Pulse:
		{
			U64 minimum_pulse_width = UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE );
			U64 spec_pulse_width = UsToSamples( SPEC_RESET_PULSE );
			U64 maximum_pulse_width = mLowPulseLength + 1;
			if( force_overdrive || mOverdrive )
			{
				minimum_pulse_width = UsToSamples( SPEC_OVD_RESET_PULSE - MARGIN_INSIDE_OVD_RESET_PULSE );
				spec_pulse_width = UsToSamples( SPEC_OVD_RESET_PULSE );
				maximum_pulse_width = UsToSamples( SPEC_MAX_OVD_RESET_PULSE + MARGIN_OUTSIDE_OVD_RESET_PULSE );
			}
			if( ( mLowPulseLength > minimum_pulse_width ) /*&& ( mLowPulseLength < maximum_pulse_width )*/ )
			{
				//Reset Pulse Detected.
				//Print Reset Bubble.
				bool flag_warning = false;
				if( force_overdrive || mOverdrive )
				{
					if( mLowPulseLength > UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE ) )
					{ 
						if( mLowPulseLength < UsToSamples( SPEC_RESET_PULSE ) )
							flag_warning = true;
					}
				}
				else
				{
					if( mLowPulseLength < spec_pulse_width )
						flag_warning = true;
				}
				mResults->CommitPacketAndStartNewPacket();
				if( flag_warning == true )
					//TODO:  RecordBubble( one_wire_bubbles, mFallingEdgeSample, mRisingEdgeSample, RestartPulse, mLowPulseLength );
					RecordFrame(mFallingEdgeSample, mRisingEdgeSample, RestartPulse, 0, true );
				else
					//TODO:  RecordBubble( one_wire_bubbles, mFallingEdgeSample, mRisingEdgeSample, RestartPulse );
					RecordFrame(mFallingEdgeSample, mRisingEdgeSample, RestartPulse );

				if (mLowPulseLength > UsToSamples( SPEC_RESET_PULSE - MARGIN_INSIDE_RESET_PULSE ))
					mOverdrive = false;

				mRomBitsRecieved = 0;
				mRomDetected = 0;
				mDataDetected = 0;
				mDataBitsRecieved = 0;

				mCurrentState = Decode; 
				mCurrentFrame = Data;
				continue;
			}
			else if (mCurrentState == NewPacketState)
			{
				mResults->CommitPacketAndStartNewPacket();
				mDataBitsRecieved = 0;
				mCurrentState = Decode; 
				mCurrentFrame = Data;
				continue;
			}
		}

		// start decoding data!
		if ( mCurrentState == Decode)
		{
			if( mDataBitsRecieved == 0 )
			{
				mDataDetected = 0;
				//Tag start of Family Code.
				mByteStartSample = mFallingEdgeSample;
			}
			
			U64 sample_location_offset = UsToSamples(SPEC_SAMPLE_CHECK_POINT);
			mOneWire->Advance( U32( sample_location_offset ) );
			
			if( mOneWire->GetBitState() == BIT_HIGH )
			{
				//invalid pulse
				mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::X, mSettings->mOneWireChannel );
			}
			else
			{
				U64 sample_location_offset = UsToSamples( SPEC_SAMPLE_POINT-SPEC_SAMPLE_CHECK_POINT );
				mOneWire->Advance( U32( sample_location_offset ) );
				
				if( mOneWire->GetBitState() == BIT_HIGH )
				{
					//short pulse, this is a 1
					mDataDetected |= U64(0x1) << mDataBitsRecieved;
					mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::One, mSettings->mOneWireChannel );
				}
				else
				{
					//long pulse, this is a 0
					mResults->AddMarker( mOneWire->GetSampleNumber(), AnalyzerResults::Zero, mSettings->mOneWireChannel );
				}
				mDataBitsRecieved += 1;

				switch ( mCurrentFrame )
				{
					case UnknownState:
						{
							//Do nothing.
						}
						break;
					
					/*case Data:
					{
						if( mDataBitsRecieved == 8 )
						{
							RecordFrame( mByteStartSample, mRisingEdgeSample, Byte, mDataDetected );
							mDataBitsRecieved = 0;
							mCurrentFrame = Data;
						}
						break;
					}*/
					case Data:
					{
						if( mDataBitsRecieved == 16 )
						{
							RecordFrame( mByteStartSample, mRisingEdgeSample, Data, mDataDetected );
							mDataBitsRecieved = 0;
							mCurrentFrame = Command;
						}
						break;
					}
					case Command:
					{
						if( mDataBitsRecieved == 48 )
						{
							RecordFrame( mByteStartSample, mRisingEdgeSample, Command, mDataDetected );
							mDataBitsRecieved = 0;
							mCurrentFrame = CRC;
						}
						break;
					}
					case CRC:
					{
						if( mDataBitsRecieved == 8 )
						{
							RecordFrame( mByteStartSample, mRisingEdgeSample, CRC, mDataDetected );
							mDataBitsRecieved = 0;
						}
						break;
					}
				}
			}
		}
		


		ReportProgress( mOneWire->GetSampleNumber() );
		CheckIfThreadShouldExit();


	}
}

void OneWireAnalyzer::RecordFrame( U64 starting_sample, U64 ending_sample, OneWireFrameType type, U64 data, bool warning )
{
	Frame frame;
	U8 flags = 0;
	if( warning == true )
		flags |= DISPLAY_AS_WARNING_FLAG;
	frame.mFlags = flags;
	frame.mStartingSampleInclusive = starting_sample;
	frame.mEndingSampleInclusive = ending_sample;
	frame.mType = (U8)type;
	frame.mData1 = data;

	mResults->AddFrame( frame );

	mResults->CommitResults();
}



U32 OneWireAnalyzer::GenerateSimulationData( U64 minimum_sample_index, U32 device_sample_rate, SimulationChannelDescriptor** simulation_channels )
{

	if( mSimulationInitilized == false )
	{
		mSimulationDataGenerator.Initialize( GetSimulationSampleRate(), mSettings.get() );
		mSimulationInitilized = true;
	}

	return mSimulationDataGenerator.GenerateSimulationData( minimum_sample_index, device_sample_rate, simulation_channels );


}
bool OneWireAnalyzer::NeedsRerun()
{
	return false;
}

U64 OneWireAnalyzer::UsToSamples( U64 us )
{

	return ( mSampleRateHz * us ) / 1000000;


}

U64 OneWireAnalyzer::SamplesToUs( U64 samples )
{
	return( samples * 1000000 ) / mSampleRateHz;
}

U32 OneWireAnalyzer::GetMinimumSampleRateHz()
{
	return 2000000;
}

const char gAnalyzerName[] = "1-Wire_mod";  //your analyzer must have a unique name

const char* OneWireAnalyzer::GetAnalyzerName() const
{
	return gAnalyzerName;
}

const char* GetAnalyzerName()
{
	return gAnalyzerName;
}

Analyzer* CreateAnalyzer()
{
	return new OneWireAnalyzer();
}

void DestroyAnalyzer( Analyzer* analyzer )
{
	delete analyzer;
}
