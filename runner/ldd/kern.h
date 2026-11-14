// kern.h - the kernel-side classes a minimal EPOC R5 logical device driver
// needs, for the netBook kernel (EKERN[100000b7], ROMs 1.05(254) and 450).
//
// From psikern/ldd/kern.h (rebuilt there for the 5mx from its ROM's own
// Video.ldd). The netBook's layouts were checked against its ROMs: the
// vtables at 0x5002a360 (DLogicalDevice) and 0x5002a444 (DLogicalChannel) in
// the 450 image have the same slots, the device's constructor sets iVersion
// at 0x14, and the channel's keeps its device at 0x2c and counts
// iOpenChannels at 0x24, as below. Keep drivers to what is declared here.
//
// Link with ekern_nb.def (the netBook's ordinals), never the SDK's
// ekern.lib or psikern's 5mx list: the ordinals differ on each kernel.
#ifndef PSI_KERN_H
#define PSI_KERN_H
#include <e32base.h>

class DLogicalChannel;

// The vtable: CObject's five slots (~, Open, Close, Name, FullName), then
// Remove, QueryVersionSupported, IsAvailable (netBook EKERN 104, 103, 102), then the
// three pure virtuals the driver supplies (Video.ldd's vtable has 11 slots).
// Size 0x28 (Video.ldd allocates 0x28 for its device and adds nothing).
class DLogicalDevice : public CObject
	{
public:
	IMPORT_C DLogicalDevice();                         // 106
	IMPORT_C ~DLogicalDevice();                        // 105
	IMPORT_C virtual TInt Remove();                    // 104: returns 0 (name from the SDK list, not proven)
	IMPORT_C virtual TBool QueryVersionSupported(const TVersion& aVer) const;  // 103
	IMPORT_C virtual TBool IsAvailable(TInt aUnit,const TDesC8* aDriver,const TDesC8* aInfo) const; // 102: returns ETrue
	virtual TInt Install()=0;                          // SetName() here
	virtual void GetCaps(TDes8& aDes) const=0;
	virtual DLogicalChannel* CreateL()=0;
public:
	TVersion iVersion;      // 0x14 (set up by the constructor)
	TUint iParseMask;       // 0x18 (Video.ldd: 6. 0 = no unit, no PDD, no info)
	TUint iUnitsMask;       // 0x1c
	TUint iReserved;        // 0x20 (not touched by any code we looked at)
	TInt iOpenChannels;     // 0x24 (DLogicalChannel's constructor and destructor count it)
	};

// The vtable: CObject's five slots, Close overridden (EKERN 87), then DoCancel
// (slot 5, pure), DoRequest (slot 6, pure), DoCreateL (slot 7, EKERN 92 is
// an empty default) and DoControl (slot 8, EKERN 91 returns KErrNotSupported).
// The kernel calls DoCancel with a request number, DoRequest after it stores
// the TRequestStatus, and DoControl with the user's three arguments.
// Size 0x30 (Video.ldd's channel is 0x34 with one pointer of its own).
class DLogicalChannel : public CObject
	{
public:
	IMPORT_C DLogicalChannel(DLogicalDevice* aDevice); // 101
	IMPORT_C ~DLogicalChannel();                       // 100
	IMPORT_C virtual void Close();                     // 87
protected:
	virtual void DoCancel(TInt aReqNo)=0;
	virtual void DoRequest(TInt aReqNo,TAny* a1,TAny* a2)=0;
	IMPORT_C virtual void DoCreateL(TInt aUnit,CBase* aPdd,const TDesC8* aInfo,const TVersion& aVer); // 92
	IMPORT_C virtual TInt DoControl(TInt aFunction,TAny* a1,TAny* a2);  // 91
public:
	TAny* iThread;               // 0x14 the client thread (DThread*)
	TRequestStatus* iStatus[4];  // 0x18 one per asynchronous request
	TUint iBehaviour;            // 0x28 which requests are allowed (SetBehaviour)
	DLogicalDevice* iDevice;     // 0x2c
	};

#endif
