#include <cmath>

#include <ardour/dB.h>
#include <ardour/route.h>

#import "PSMixerStrip.h"
#import "PSFastMeter.h"
#import "logmeter.h"

@implementation PSMixerStrip

- (id)initWithRoute:(boost::shared_ptr<ARDOUR::Route>)r withSession:(ARDOUR::Session*)s
{
	self = [super initWithRoute:r withSession:s];
	if (self) {
		[NSBundle loadNibNamed:@"MixerStrip" owner:self];
		[NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(updateMeters) userInfo:nil repeats:YES];
		
		maxPeak = minus_infinity();
	}
	
	return self;
}

- (void)dealloc
{
	[meterArray release];
	[stripView release];
	[super dealloc];
}

- (void)awakeFromNib
{
	[super awakeFromNib];
	[self setupMeters];
}

- (void)setupMeters
{
	uint32_t nmeters;
	
	switch (route->meter_point()) {
		case ARDOUR::MeterPreFader:
		case ARDOUR::MeterInput:
			nmeters = route->n_inputs();
		case ARDOUR::MeterPostFader:
		default:
			nmeters = route->n_outputs();
	}
	
	if (nmeters == 0) {
		return;
	}
	
	if (nmeters <= 2) {
		meterWidth = 5;  // regular_meter_width
	} else {
		meterWidth = 2;  // thin_meter_width
	}

	[meterArray release];
	meterArray = [[NSMutableArray alloc] initWithCapacity:2];

	int startPoint = 47 + (46/2) - ((nmeters/2)*(meterWidth+2));  // center meters
	for (int i = 0; i < nmeters; ++i) {
		NSRect newRect = NSMakeRect(startPoint+([meterArray count]*(meterWidth+2)), 234, meterWidth, 274);
		PSFastMeter *newMeter = [[PSFastMeter alloc] initWithFrame:newRect];
		[newMeter setHoldCount:ARDOUR::Config->get_meter_hold()];
		[stripView addSubview:newMeter];
		[meterArray addObject:newMeter];
	}
}

- (float)gain
{
	return gain;
}

- (void)setGain:(float)g
{
	float f = std::min (g, 6.0f);
	route->set_gain (dB_to_coefficient(f), self);
	
	gain = f;
}

- (NSView*)stripView
{
	return stripView;
}

- (void)updateMeters
{
	NSEnumerator *en = [meterArray objectEnumerator];
	
	PSFastMeter *fm = nil;
	int index = 0;
	float peak, mpeak;
	while (fm = [en nextObject]) {
		peak = route->peak_input_power (index);
		[fm setLevel:log_meter(peak)];
		
		mpeak = route->max_peak_power (index);
		
		if (mpeak > maxPeak) {
			maxPeak = mpeak;
			
			if (maxPeak <= -200.0f) {
				[peakField setStringValue:@"-inf"];
			} else {
				[peakField setFloatValue:mpeak];
			}
		}
		
		++index;
	}
}

@end
