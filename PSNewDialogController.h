/* NewDialogController */

#import <Cocoa/Cocoa.h>

@interface PSNewDialogController : NSObject
{
    IBOutlet NSView *accessoryView;
    IBOutlet NSPopUpButton *templates;
	
	int nMasters;
	int nMonitors;
	BOOL masterBusOn;
	BOOL monitorBusOn;
	BOOL autoPhysIn;
	BOOL useOnlyPhysicalInputsOn;
	int nPhysicalInputs;
	BOOL autoOuts;
	int outputChoice;
	BOOL useOnlyPhysicalOutputsOn;
	int nPhysicalOutputs;
}

- (int)nMasters;
- (void)setNMasters:(int)n;
- (int)nMonitors;
- (void)setNMonitors:(int)n;
- (BOOL)masterBusOn;
- (void)setMasterBusOn:(BOOL)yn;
- (BOOL)monitorBusOn;
- (void)setMonitorBusOn:(BOOL)yn;
- (BOOL)autoPhysIn;
- (void)setAutoPhysIn:(BOOL)yn;
- (BOOL)useOnlyPhysicalInputsOn;
- (void)setUseOnlyPhysicalInputsOn:(BOOL)yn;
- (int)nPhysicalInputs;
- (void)setNPhysicalInputs:(int)n;
- (BOOL)autoOuts;
- (void)setAutoOuts:(BOOL)yn;
- (int)outputChoice;
- (void)setOutputChoice:(int)n;
- (BOOL)useOnlyPhysicalOutputsOn;
- (void)setUseOnlyPhysicalOutputsOn:(BOOL)yn;
- (int)nPhysicalOutputs;
- (void)setNPhysicalOutputs:(int)n;

- (NSString*)chosenTemplate;
- (NSURL *)newFile;
- (BOOL)useTemplate;

@end
