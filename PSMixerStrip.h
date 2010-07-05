/* PSMixerStrip */

#import <Cocoa/Cocoa.h>

#import "PSRouteUI.h"

@interface PSMixerStrip : PSRouteUI
{
	IBOutlet NSColorWell *color;
	IBOutlet NSView *comments;
	IBOutlet NSButton *expand;
	IBOutlet NSView *fader;
	IBOutlet NSButton *inputBtn;
	IBOutlet NSButton *outputBtn;
	IBOutlet NSView *panners;
	IBOutlet NSTableColumn *postFaderList;
	IBOutlet NSTableColumn *preFaderList;
	IBOutlet NSTextField *gainField;
	IBOutlet NSTextField *peakField;
	IBOutlet NSView *stripView;
	
	float gain;
	float maxPeak;
	unsigned int meterWidth;
	
	NSMutableArray *meterArray;
}

- (void)setupMeters;

- (float)gain;
- (void)setGain:(float)g;

- (NSView*)stripView;

- (void)updateMeters;

@end
