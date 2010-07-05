#include <list>
#include <string>

#include <ardour/session.h>

#import "PSNewDialogController.h"

using namespace std;

static int intSort(id num1, id num2, void *context);

@implementation PSNewDialogController

- (id)init
{
	self = [super init];
	
	[NSBundle loadNibNamed:@"NewSessionWindow" owner:self];
	
	[self setNMasters:2];
	[self setNMonitors:2];
	[self setMasterBusOn:YES];
	[self setMonitorBusOn:NO];
	[self setAutoPhysIn:YES];
	[self setUseOnlyPhysicalInputsOn:NO];
	[self setNPhysicalInputs:2];
	[self setAutoOuts:YES];
	[self setOutputChoice:0];
	[self setUseOnlyPhysicalOutputsOn:NO];
	[self setNPhysicalOutputs:2];
	
	NSArray *templatePaths = [[NSBundle mainBundle] pathsForResourcesOfType:@"template" inDirectory:@"templates"];
	NSMutableArray *templateNames = [NSMutableArray arrayWithCapacity:[templatePaths count]];
	NSEnumerator *key = [templatePaths objectEnumerator];
	NSString *path;
	while (path = [key nextObject]) {
		[templateNames addObject:[[path lastPathComponent] stringByDeletingPathExtension]];  // strip path, then remove .template suffix.
	}
	
	NSArray *sortedTemplate = [templateNames sortedArrayUsingFunction:intSort context:NULL];
	[templates addItemsWithTitles:sortedTemplate];
	
	return self;
}

- (void) dealloc {
	[accessoryView release];
	
	[super dealloc];
}


- (int)nMasters
{
	return nMasters;
}

- (void)setNMasters:(int)n
{
	nMasters = n;
}

- (int)nMonitors
{
	return nMonitors;
}

- (void)setNMonitors:(int)n
{
	nMonitors = n;
}

- (BOOL)masterBusOn
{
	return masterBusOn;
}

- (void)setMasterBusOn:(BOOL)yn
{
	masterBusOn = yn;
}

- (BOOL)monitorBusOn
{
	return monitorBusOn;
}

- (void)setMonitorBusOn:(BOOL)yn
{
	monitorBusOn = yn;
}

- (BOOL)autoPhysIn
{
	return autoPhysIn;
}

- (void)setAutoPhysIn:(BOOL)yn
{
	autoPhysIn = yn;
}

- (BOOL)useOnlyPhysicalInputsOn
{
	return useOnlyPhysicalInputsOn;
}

- (void)setUseOnlyPhysicalInputsOn:(BOOL)yn
{
	useOnlyPhysicalInputsOn = yn;
}

- (int)nPhysicalInputs
{
	return nPhysicalInputs;
}

- (void)setNPhysicalInputs:(int)n
{
	nPhysicalInputs = n;
}

- (BOOL)autoOuts
{
	return autoOuts;
}

- (void)setAutoOuts:(BOOL)yn
{
	autoOuts = yn;
}

- (int)outputChoice
{
	return outputChoice;
}

- (void)setOutputChoice:(int)n
{
	outputChoice = n;
}

- (BOOL)useOnlyPhysicalOutputsOn
{
	return useOnlyPhysicalOutputsOn;
}

- (void)setUseOnlyPhysicalOutputsOn:(BOOL)yn
{
	useOnlyPhysicalOutputsOn = yn;
}

- (int)nPhysicalOutputs
{
	return nPhysicalOutputs;
}

- (void)setNPhysicalOutputs:(int)n
{
	nPhysicalOutputs = n;
}

- (NSURL *)newFile
{
	int result;
	NSSavePanel *sPanel = [NSSavePanel savePanel];
	
	[sPanel setTitle:@"New Session"];
	[sPanel setNameFieldLabel:@"Name:"];
	[sPanel setPrompt:@"Create"];
	[sPanel setRequiredFileType:@"gusto"];
	[sPanel setAccessoryView:accessoryView];
	result = [sPanel runModalForDirectory:NSHomeDirectory() file:nil];
	if (result == NSOKButton) {
		return [sPanel URL];
	}
	
	return @"";
}

- (NSString *)chosenTemplate
{
	return [templates titleOfSelectedItem];
}

- (BOOL)useTemplate
{
	return [templates indexOfSelectedItem] != 0;
}

@end

int intSort(id num1, id num2, void *context)
{
	int v1 = [num1 intValue];
	int v2 = [num2 intValue];
	if (v1 < v2) {
		return NSOrderedAscending;
	} else if (v1 > v2) {
		return NSOrderedDescending;
	} else {
		return NSOrderedSame;
	}
}
