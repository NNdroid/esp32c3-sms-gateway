import sys
sys.stdout.reconfigure(encoding='utf-8')

def bcd_to_iccid(bcd_bytes):
    out = []
    for b in bcd_bytes:
        hi = (b >> 4) & 0x0F
        lo = b & 0x0F
        if hi <= 9:
            out.append(str(hi))
        if lo <= 9:
            out.append(str(lo))
    return ''.join(out)

def iccid_to_bcd(iccid_str):
    digits = len(iccid_str)
    bcd = [0xFF] * 10
    for i in range(0, digits, 2):
        hi = int(iccid_str[i])
        lo = int(iccid_str[i+1]) if i+1 < digits else 0x0F
        bcd[i//2] = (hi << 4) | lo
    return bcd

# Test the old (wrong) encoding
def iccid_to_bcd_old(iccid_str):
    digits = len(iccid_str)
    bcd = [0xFF] * 10
    for i in range(0, digits, 2):
        lo = int(iccid_str[i])
        hi = int(iccid_str[i+1]) if i+1 < digits else 0x0F
        bcd[i//2] = (hi << 4) | lo
    return bcd

test_iccids = ['89883010000012345678', '8988301000001234567', '89014103211118510720']
for iccid in test_iccids:
    # New encoding
    bcd_new = iccid_to_bcd(iccid)
    decoded_new = bcd_to_iccid(bcd_new)
    print(f'ICCID: {iccid}')
    print(f'  NEW BCD: {" ".join(f"{b:02X}" for b in bcd_new)}')
    print(f'  NEW decoded: {decoded_new}  match={iccid == decoded_new}')
    
    # Old encoding
    bcd_old = iccid_to_bcd_old(iccid)
    decoded_old = bcd_to_iccid(bcd_old)
    print(f'  OLD BCD: {" ".join(f"{b:02X}" for b in bcd_old)}')
    print(f'  OLD decoded: {decoded_old}  match={iccid == decoded_old}')
    
    # What if the module expects something else?
    # Try: first digit in low nibble, second in high nibble (swapped within byte)
    bcd_swapped = iccid_to_bcd(iccid)
    decoded_swapped = bcd_to_iccid(bcd_swapped)
    print()
