"""Small ELF64 audit for authored boot artifacts (no external binutils)."""
import struct

def inspect(path,arch):
    data=path.read_bytes()
    if data[:6]!=b"\x7fELF\x02\x01":raise ValueError("Expected little-endian ELF64")
    kind,machine=struct.unpack_from("<HH",data,16)
    if machine!={"aarch64":183,"x86_64":62}[arch]:raise ValueError("Wrong machine")
    shoff=struct.unpack_from("<Q",data,40)[0];shsize,shcount=struct.unpack_from("<HH",data,58)
    sections=[struct.unpack_from("<IIQQQQIIQQ",data,shoff+i*shsize) for i in range(shcount)]
    def string(section,offset):
        start=section[4]+offset
        return data[start:data.index(b"\0",start)].decode()
    imports=[];exports=[];needed=[]
    for s in sections:
        if s[1]==11:
            for offset in range(s[4],s[4]+s[5],s[9]):
                name,info,other,index,_,_=struct.unpack_from("<IBBHQQ",data,offset)
                if not name:continue
                value=string(sections[s[6]],name)
                if not index:imports.append(value)
                elif info>>4 in (1,2) and other&3==0:exports.append(value)
        if s[1]==6:
            for offset in range(s[4],s[4]+s[5],16):
                tag,value=struct.unpack_from("<qQ",data,offset)
                if tag==1:needed.append(string(sections[s[6]],value))
    phoff=struct.unpack_from("<Q",data,32)[0];phsize,phcount=struct.unpack_from("<HH",data,54)
    stack=[];interpreter=False
    for i in range(phcount):
        ptype,flags=struct.unpack_from("<II",data,phoff+i*phsize)
        if ptype==1 and flags&3==3:raise ValueError("Writable executable segment")
        if ptype==3:interpreter=True
        if ptype==0x6474e551:stack.append(flags)
    if len(stack)!=1 or stack[0]&1:raise ValueError("Missing/unsafe stack contract")
    return dict(type=kind,machine=machine,imports=sorted(imports),exports=sorted(exports),needed=needed,interpreter=interpreter,executable_stack=False)
