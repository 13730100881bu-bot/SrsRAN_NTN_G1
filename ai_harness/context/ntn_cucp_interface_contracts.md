# NTN CU-CP Interface Contracts

## RRC and SIB19

CU-CP may produce an NTN assistance snapshot and pass it through approved
RRC/CU-CP packaging paths. The snapshot may include ephemeris, Common TA,
Koffset, Kmac, UL sync validity, reference location, t-Service, and neighbour
satellite assistance.

CU-CP must not implement DU-owned system information scheduling or broadcasting.
If a task needs DU SI scheduling, stop and request an exact non-CU-CP exception.

## F1AP-CU

CU-CP may use approved F1AP-CU contracts to carry NTN slot request intent for
loaded beams. The accepted helper is `f1ap_ntn_ul_slot_resource_request`.

The request carries control-plane intent only. It does not prove DU PUCCH/SRS
resource allocation or MAC scheduler behavior.

## NGAP

CU-CP may implement NGAP-facing NTN user location reporting, AMF
LocationReportingControl handling, area-of-interest reporting, change of serving
cell reporting, Mapped Cell, derived TAC, TAI, and timestamped location.

NGAP changes must remain in accepted NGAP control-plane paths or be listed as
exact task-level exception paths.

## E1AP and PDU session policy

CU-CP may use UE/DRB, QoS, ARP, slice, and emergency-priority information when
ordering loaded beams or choosing drain/release behavior. E1AP changes are not
generally open and require task-specific exact path justification.

## Forbidden execution layers

The following are not CU-CP contracts and must not be implemented in this
harness without explicit owner approval:

- O-DU and flexible_o_du behavior.
- DU-high or DU-low behavior.
- MAC scheduler behavior.
- HARQ timing execution.
- TA scheduler behavior.
- PRACH behavior.
- PHY or lower PHY.
- RU/RF/radio driver behavior.
- ZMQ channel behavior.
- GIS-site behavior.
