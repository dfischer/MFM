#include "T2ITC.h"
#include "T2Tile.h"

#include <sys/types.h> 
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>    // For close
#include <stdio.h>     // For snprintf
#include <errno.h>     // For errno

#include "dirdatamacro.h" 

namespace MFM {

  T2ITCPacketPoller::T2ITCPacketPoller(T2Tile& tile)
    : mTile(tile)
    , mIteration(tile.getRandom(), 100)
  { }

  void T2ITCPacketPoller::onTimeout(TimeQueue& srcTq) {
    for (ITCIterator itr = mIteration.begin(); itr.hasNext(); ) {
      u32 dir6 = itr.next();
      T2ITC & itc = mTile.getITC(dir6);
      itc.pollPackets(true);
    }

    schedule(srcTq,10);
  }

  bool T2ITC::isGingerDir6(Dir6 dir6) {
    switch (dir6) {
#define XX(DC,fr,p1,p2,p3,p4) case DIR6_##DC: return !fr;
      DIRDATAMACRO()
#undef XX
    default:
        MFM_API_ASSERT_ARG(0);
    }
  }

  void T2ITC::scheduleWait(WaitCode wc) {
    u32 ms;
    TimeQueue & tq = mTile.getTQ();
    Random & random = mTile.getRandom();
    switch (wc) {
    case WC_NOW:    ms = 0u; break;
    case WC_HALF:   ms = WC_HALF_MS; break;
    case WC_FULL:   ms = WC_FULL_MS; break;
    case WC_LONG:   ms = random.Between(WC_LONG_MIN_MS,WC_LONG_MAX_MS); break;
    case WC_RANDOM: ms = random.Between(WC_RANDOM_MIN_MS,WC_RANDOM_MAX_MS); break;
    case WC_RANDOM_SHORT:
                    ms = random.Between(WC_RANDOM_SHORT_MIN_MS,WC_RANDOM_SHORT_MAX_MS); break;
    default: FAIL(ILLEGAL_ARGUMENT);
    }
    schedule(tq,ms,0);
  }

  bool T2ITC::isVisibleUsable() {
    T2ITCStateOps & ops = getT2ITCStateOps();
    return ops.isVisibleUsable();
  }

  bool T2ITC::isCacheUsable() {
    T2ITCStateOps & ops = getT2ITCStateOps();
    return ops.isCacheUsable();
  }

  const Rect & T2ITC::getVisibleRect() const { return mTile.getVisibleRect(mDir6); }
  const Rect & T2ITC::getCacheRect() const { return mTile.getCacheRect(mDir6); }
  const Rect & T2ITC::getVisibleAndCacheRect() const { return mTile.getVisibleAndCacheRect(mDir6); }

  Rect T2ITC::getRectForTileInit(Dir6 dir6, u32 widthIn, u32 skipIn) {
    switch (dir6) {
    case DIR6_ET:
      return Rect(T2TILE_WIDTH-(widthIn+skipIn),0,
                  widthIn,T2TILE_HEIGHT);
    case DIR6_SE:
      return Rect(T2TILE_WIDTH/2-CACHE_LINES,T2TILE_HEIGHT-(widthIn+skipIn),
                  T2TILE_WIDTH/2+CACHE_LINES, widthIn);
    case DIR6_SW:
      return Rect(0,T2TILE_HEIGHT-(widthIn+skipIn),
                  T2TILE_WIDTH/2+CACHE_LINES, widthIn);
    case DIR6_WT:
      return Rect(skipIn,0,
                  widthIn,T2TILE_HEIGHT);
    case DIR6_NW:
      return Rect(0,skipIn,
                  T2TILE_WIDTH/2+CACHE_LINES, widthIn);
    case DIR6_NE:
      return Rect(T2TILE_WIDTH/2-CACHE_LINES, skipIn,
                  T2TILE_WIDTH/2+CACHE_LINES, widthIn);
    }
    FAIL(ILLEGAL_STATE);
  }

  void T2ITC::onTimeout(TimeQueue& srcTq) {
    T2ITCStateOps & ops = getT2ITCStateOps();
    const u32 curenable = mTile.getKITCPoller().getKITCEnabledStatus(mDir8);
    const u32 minenable = ops.getMinimumEnabling();
    if (curenable < minenable) {
      LOG.Warning("%s enable=%d need=%d, resetting",
                  getName(), curenable, minenable);
      reset();
    } else {
      T2PacketBuffer pb;
      pb.Printf("%c%c",
                0xa0|mDir8,   /*standard+urgent to dir8*/
                0xf0|mStateNumber /* mfm+XITC==ITCcmd + sn*/
                );
      ops.timeout(*this, pb, srcTq);
    }
  }

  void T2ITC::pollPackets(bool dispatch) {
    if (mFD < 0) return; // Not open yet
    while (tryHandlePacket(dispatch)) { }
  }

  u32 T2ITC::getCompatibilityStatus() {
    return mTile.getKITCPoller().getKITCEnabledStatus(mDir8);
  }

  bool T2ITC::tryHandlePacket(bool dispatch) {
    T2PacketBuffer pb;
    const u32 MAX_PACKET_SIZE = 255;
    int len = pb.Read(mFD, MAX_PACKET_SIZE);
    const char * packet = pb.GetBuffer();
    if (len < 0) {
      if (errno == EAGAIN) return false;
      if (errno == ERESTART) return true; // try me again
      LOG.Error("%s: Read failed on mFD %d: %s",
                getName(), mFD, strerror(errno));
      return false;
    }
    if (!dispatch) return true;
    if (len < 2) LOG.Warning("%s one byte 0x%02x packet, ignored", getName(), packet[0]);
    else {
      u32 xitc = (packet[1]>>4)&0x7;
      if (xitc == 7) { // ITC state command
        ITCStateNumber psn = (ITCStateNumber) (packet[1]&0xf);
        T2ITCStateOps * ops = T2ITCStateOps::getOpsOrNull(psn);
        MFM_API_ASSERT_NONNULL(ops);
        ops->receive(*this, pb, mTile.getTQ());
      } else if (xitc <= 4) {
        LOG.Warning("%s xitc circuit %d packet, ignored", getName(), xitc);
      } else {
        LOG.Warning("%s xitc reserved %d packet, ignored", getName(), xitc);
      }
    }
    return true;
  }

  bool T2ITC::trySendPacket(T2PacketBuffer & pb) {
    s32 packetlen = pb.GetLength();
    const char * bytes = pb.GetBuffer();
    s32 len = ::write(mFD, bytes, packetlen);
    LOG.Message("  %s wrote %d == %d",getName(),packetlen,len);
    if (len < 0) {
      if (errno == EAGAIN) return false; // No room, try again
      if (errno == ERESTART) return false; // Interrupted, try again
    }
    if (len != packetlen) return false;  // itcpkt LKM doesn't actually support partial write
    return true;                         // 'the bird is away'
  }

  void T2ITC::setITCSN(ITCStateNumber itcsn) {
    MFM_API_ASSERT_ARG(itcsn >= 0 && itcsn < MAX_ITC_STATE_NUMBER);
    mStateNumber = itcsn;
    LOG.Message("->%s",getName());
  }

  void T2ITC::reset() {
    if (mFD >= 0) close();
    resetAllCircuits();
    setITCSN(ITCSN_INIT);
    schedule(mTile.getTQ(),500); // Delay in case reset-looping
  }

  bool T2ITC::initialize() {
    int ret = open();
    if (ret < 0) {
      LOG.Error("%s open failed: %s", getName(), strerror(ret));
      return false;
    }
    pollPackets(false); // First flush packets

    return true;
    /*
    T2PacketBuffer pb;    // Then try to ping for status
    pb.WriteByte(0xc0|LCLSTD_MFM_TO_LKM_PING); 

    return trySendPacket(pb);
    */
  }

  void T2ITC::resetAllCircuits() {
    for (u8 i = 0; i < CIRCUIT_COUNT; ++i) {
      mActiveFree[i] = i;
      for (u8 act = 0; act <= 1; ++act) {
        mCircuits[act][i].mNumber = i;
        mCircuits[act][i].mEW = 0;  // Unassigned
      }
    }
  }

  T2ITC::T2ITC(T2Tile& tile, Dir6 dir6, const char * name)
    : mTile(tile)
    , mDir6(dir6)
    , mDir8(mapDir6ToDir8(dir6))
    , mName(name)
    , mStateNumber(ITCSN_INIT)
    , mActiveFreeCount(CIRCUIT_COUNT)
    , mFD(-1)
  {
    reset();
  }

  /**** LATE ITC STATES HACKERY ****/

  /*** DEFINE STATEOPS SINGLETONS **/
#define XX(NAME,VIZ,CCH,MINCOMP,CUSTO,CUSRC,STUB,DESC) static T2ITCStateOps_##NAME singletonT2ITCStateOps_##NAME;
  ALL_ITC_STATES_MACRO()
#undef XX

  /*** DEFINE ITCSTATENUMBER -> STATEOPS MAPPING **/
  T2ITCStateOps::T2ITCStateArray T2ITCStateOps::mStateOpsArray = {
#define XX(NAME,VIZ,CCH,MINCOMP,CUSTO,CUSRC,STUB,DESC) &singletonT2ITCStateOps_##NAME,
  ALL_ITC_STATES_MACRO()
#undef XX
    0
  };

  T2ITCStateOps * T2ITCStateOps::getOpsOrNull(ITCStateNumber sn) {
    if (sn >= MAX_ITC_STATE_NUMBER) return 0;
    return mStateOpsArray[sn];
  }

  /*** DEFINE STUBBED-OUT STATE METHODS **/
#define YY00(NAME,FUNC) 
#define YY01(NAME,FUNC) 
#define YY10(NAME,FUNC) 
#define YY11(NAME,FUNC)                                                 \
void T2ITCStateOps_##NAME::FUNC(T2ITC & ew, T2PacketBuffer & pb, TimeQueue& tq) { \
  LOG.Error("%s is a stub", __PRETTY_FUNCTION__);                                 \
  DIE_UNIMPLEMENTED();                                                  \
}                                                                       \
 
#define XX(NAME,VIZ,CCH,MINCOMP,CUSTO,CUSRC,STUB,DESC) \
  YY##CUSTO##STUB(NAME,timeout)                        \
  YY##CUSRC##STUB(NAME,receive)                        \
  
  ALL_ITC_STATES_MACRO()
#undef XX
#undef YY11
#undef YY10
#undef YY01
#undef YY00

  /*** STATE NAMES AS STRING **/
  const char * itcStateName[] = {
#define XX(NAME,VIZ,CCH,MINCOMP,CUSTO,CUSRC,STUB,DESC) #NAME,
  ALL_ITC_STATES_MACRO()
#undef XX
  "?ILLEGAL"
  };

  const char * getITCStateName(ITCStateNumber sn) {
    if (sn >= MAX_ITC_STATE_NUMBER) return "illegal";
    return itcStateName[sn];
  }

  /*** STATE DESCRIPTIONS AS STRING **/
  const char * itcStateDesc[] = {
#define XX(NAME,VIZ,CCH,MINCOMP,CUSTO,CUSRC,STUB,DESC) DESC,
  ALL_ITC_STATES_MACRO()
#undef XX
  "?ILLEGAL"
  };

  const char * getITCStateDescription(ITCStateNumber sn) {
    if (sn >= MAX_ITC_STATE_NUMBER) return "illegal";
    return itcStateDesc[sn];
  }

  const char * T2ITC::getName() const {
    static char buf[100];
    snprintf(buf,100,"ITC/%s:%s",
             mName,
             getITCStateName(mStateNumber));
    return buf;
  }

  //// DEFAULT HANDLERS FOR T2ITCStateOps
  void T2ITCStateOps::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    LOG.Warning("%s default mode timeout, resetting",itc.getName());
    itc.reset();
  }

  void T2ITCStateOps::receive(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    LOG.Warning("%s default mode receive packet len %d , resetting",
                itc.getName(), pb.GetLength());
    itc.reset();
  }

  T2ITCStateOps & T2ITC::getT2ITCStateOps() {
    if (mStateNumber >= T2ITCStateOps::mStateOpsArray.size())
      FAIL(ILLEGAL_STATE);
    T2ITCStateOps * ops = T2ITCStateOps::mStateOpsArray[mStateNumber];
    if (!ops)
      FAIL(ILLEGAL_STATE);
    return *ops;
  }

  CircuitNum T2ITC::tryAllocateActiveCircuit() {
    if (mActiveFreeCount==0) return ALL_CIRCUITS_BUSY;
    Random & r = mTile.getRandom();
    u8 idx = r.Between(0,mActiveFreeCount-1);
    CircuitNum ret = mActiveFree[idx];
    mActiveFree[idx] = mActiveFree[--mActiveFreeCount];
    return ret;
  }

  u32 T2ITC::activeCircuitsInUse() const {
   return CIRCUIT_COUNT - mActiveFreeCount;
  }
  void T2ITC::freeActiveCircuit(CircuitNum cn) {
    assert(cn < CIRCUIT_COUNT);
    assert(mActiveFreeCount < CIRCUIT_COUNT);
    mActiveFree[mActiveFreeCount++] = cn;
    mCircuits[1][cn].mEW = 0;
    /* let it go to timeout, they're doing ping-pong
    if (activeCircuitsInUse()==0 && getITCSN() == ITCSN_FOLLOW)
      bump();
    */
  }

  const char * T2ITC::path() const {
    static char buf[100];
    snprintf(buf,100,"/dev/itc/mfm/%s",mName);
    return buf;
  }

  int T2ITC::open() {
    int ret = ::open(path(),O_RDWR|O_NONBLOCK);
    if (ret < 0) return -errno;
    mFD = ret;
    return ret;
  }

  int T2ITC::close() {
    int ret = ::close(mFD);
    mFD = -1;
    if (ret < 0) return -errno;
    return ret;
  }

  /////////////////// CUSTOM T2ITCStateOps HANDLERS

  void T2ITCStateOps_INIT::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    LOG.Message("%s: Initializing",itc.getName());
    if (itc.initialize()) {
      itc.setITCSN(ITCSN_WAITCOMP);
      itc.scheduleWait(WC_NOW);
    } else itc.reset();
  }

  void T2ITCStateOps_WAITCOMP::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.getCompatibilityStatus() >= 2) {
      // Go for control of this negotiation
      Random & random = itc.mTile.getRandom();
      itc.setITCSN(random.CreateBool() ? ITCSN_LEAD0 : ITCSN_LEAD1); // Yoink!
      itc.scheduleWait(WC_NOW);
    } else {
      itc.schedule(tq,1000);
    }
  }

  s32 T2ITC::resolveLeader(ITCStateNumber theirsn) {
    MFM_API_ASSERT_ARG(theirsn == ITCSN_WLEAD0 || theirsn == ITCSN_WLEAD1);
    ITCStateNumber usn = getITCSN();
    if (usn <= ITCSN_WAITCOMP) return -1; // They yoinked us
    if (usn >= ITCSN_FOLLOW) return 0;    // Illegal state need reset
    if (usn == ITCSN_LEAD0) usn = ITCSN_WLEAD0; // Impute later states if they
    else if (usn == ITCSN_LEAD1) usn = ITCSN_WLEAD1; // caught us right out of waitcomp
    bool evens = (usn == theirsn);
    bool ginger = isGinger();
    return (ginger == evens) ? 1 : -1;    // ginger wins on evens
  }

  void T2ITC::leadFollowOrReset(ITCStateNumber theirimputedsn) {
    s32 raceresult = resolveLeader(theirimputedsn);
    if (raceresult < 0) {              // They yoinked us or we raced and they won
      setITCSN(ITCSN_FOLLOW);          // either way, we follow
      scheduleWait(WC_HALF);
    } else if (raceresult > 0) {
      setITCSN(ITCSN_WFOLLOW);  // it was a race and we won
      scheduleWait(WC_FULL);
    } else
      reset();                  // Something messed up
  }

  void T2ITCStateOps_LEAD0::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.trySendPacket(pb)) {
      itc.setITCSN(ITCSN_WLEAD0);
      itc.scheduleWait(WC_FULL);
    } else itc.scheduleWait(WC_RANDOM_SHORT);
  }

  void T2ITCStateOps_LEAD0::receive(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    itc.leadFollowOrReset(ITCSN_WLEAD0);  // End up in FOLLOW or WFOLLOW
  }

  void T2ITCStateOps_LEAD1::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.trySendPacket(pb)) {
      itc.setITCSN(ITCSN_WLEAD1);
      itc.scheduleWait(WC_FULL);
    } else itc.scheduleWait(WC_RANDOM_SHORT);
  }

  void T2ITCStateOps_LEAD1::receive(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    itc.leadFollowOrReset(ITCSN_WLEAD1);// End up in FOLLOW or WFOLLOW
  }

  void T2ITC::startCacheSync(bool asLeader) {
    mVisibleAtomsToSend.begin(getVisibleRect());
    setITCSN(asLeader ? ITCSN_CACHEXG : ITCSN_WCACHEXG);
    scheduleWait(WC_NOW);
  }

  bool T2ITC::sendCacheAtoms(T2PacketBuffer & pb) {
    const u32 BYTES_PER_ATOM = OurT2Atom::BPA/8; // Bits Per Atom/Bits Per Byte
    const u32 BYTES_PER_DIM = 1; // Sending u8 dimensions inside visibleRect
    const u32 BYTES_PER_COORD = 2*BYTES_PER_DIM;
    const u32 BYTES_PER_SITE = BYTES_PER_COORD+BYTES_PER_ATOM;
    Sites & sites = mTile.getSites();
    UPoint pos = MakeUnsigned(mVisibleAtomsToSend.getRect().GetPosition());
    RectIterator hold = mVisibleAtomsToSend;
    bool alreadyDone = !mVisibleAtomsToSend.hasNext();
    while (mVisibleAtomsToSend.hasNext()) {
      if (pb.CanWrite() < (s32) BYTES_PER_SITE) break;
      SPoint coord = mVisibleAtomsToSend.next();
      UPoint ucoord = MakeUnsigned(coord);
      UPoint coordInVis = ucoord - pos;
      pb.Printf("%c%c",(u8) coordInVis.GetX(), (u8) coordInVis.GetY());
      
      OurT2Site & site = sites.get(ucoord);
      OurT2Atom & atom = site.GetAtom();
      const OurT2AtomBitVector & bv = atom.GetBits();
      bv.PrintBytes(pb);
    }
    if (trySendPacket(pb)) return alreadyDone; // true when we just sent an empty CACHEXG
    // Grrr
    mVisibleAtomsToSend = hold; // All that work.
    return false;
  }

  bool T2ITC::tryReadAtom(ByteSource & in, UPoint & where, OurT2AtomBitVector & bv) {
    u8 x,y;
    if (in.Scanf("%c%c",&x,&y) != 2) return false;
    OurT2AtomBitVector tmpbv;
    if (!tmpbv.ReadBytes(in)) return false;
    where.SetX(x);
    where.SetY(y);
    bv = tmpbv;
    return true;
  }

  bool T2ITC::recvCacheAtoms(T2PacketBuffer & pb) {
    CharBufferByteSource cbs = pb.AsByteSource();
    u8 byte0=0, byte1=0;
    if ((cbs.Scanf("%c%c",&byte0,&byte1) != 2) ||
        ((byte0 & 0xf0) != 0xa0) ||
        (byte1 != (0xf0|ITCSN_CACHEXG))) {
      LOG.Error("%s: Bad CACHEXG hdr 0x%02x 0x%02x",
                getName(), byte0, byte1);
      reset();
      return true;
    }

    Sites & sites = mTile.getSites();
    const Rect cache = getCacheRect();
    const SPoint pos = cache.GetPosition();
    u32 count = 0;
    UPoint offset;
    OurT2Atom tmpatom;
    OurT2AtomBitVector & tmpbv = tmpatom.GetBits();

    while (tryReadAtom(cbs, offset, tmpbv)) {
      SPoint dest = pos+MakeSigned(offset);
      if (!cache.Contains(dest)) {
        LOG.Error("%s: (%d,%d) ATTEMPT TO ESCAPE CACHE",
                  getName(), dest.GetX(), dest.GetY());
        reset();
        return true;
      }
      if (!tmpatom.IsSane()) {
        LOG.Error("%s: RECEIVED INSANE ATOM",getName());
        reset();
        return true;
      }
      OurT2Site & site = sites.get(MakeUnsigned(dest));
      OurT2Atom & atom = site.GetAtom();
      atom = tmpatom;  // BAM
      ++count;
    }
    return count == 0; // CACHEXG w/no atoms means end
  }

  // Follower times out early, ships FOLLOW
  void T2ITCStateOps_FOLLOW::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.activeCircuitsInUse() > 0) // Shouldn't be, by now
      itc.reset();
    else if (itc.sendCacheAtoms(pb)) {  // True when all atoms shipped
      itc.setITCSN(ITCSN_WCACHEXG);  // Else wait until we've sent all ours
      itc.scheduleWait(WC_RANDOM_SHORT);
    } else itc.scheduleWait(WC_RANDOM_SHORT);
#if 0
    if (itc.activeCircuitsInUse() == 0 && itc.trySendPacket(pb)) {
      itc.startCacheSync(true);
    } else 
      itc.reset();
#endif
  }

  // Leader receives FOLLOW, starts sending cache, goes to CACHEXG
  void T2ITCStateOps_FOLLOW::receive(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.getITCSN() != ITCSN_WFOLLOW || itc.activeCircuitsInUse() > 0) itc.reset();
    else {
      itc.startCacheSync(true);
    }
  }

  void T2ITCStateOps_WFOLLOW::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    FAIL(INCOMPLETE_CODE);
  }

  // Leader remains in CACHEXG unless all atoms sent, then goes to ??
  void T2ITCStateOps_CACHEXG::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.sendCacheAtoms(pb)) {  // True when all atoms shipped
      itc.setITCSN(ITCSN_WCACHEXG);  // Else wait until we've sent all ours
    }
    itc.scheduleWait(WC_RANDOM_SHORT);
  }

  void T2ITCStateOps_CACHEXG::receive(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    ITCStateNumber sn = itc.getITCSN();
    if (sn < ITCSN_FOLLOW || sn > ITCSN_WCACHEXG) { itc.reset(); return; }
    if (itc.recvCacheAtoms(pb)) { // True when all atoms arrived, or an error reset
      if (itc.getITCSN() == ITCSN_INIT) return; // just bail if something went wrong
      itc.setITCSN(ITCSN_WCACHEXG);  // Else wait until we've sent all ours
      itc.scheduleWait(WC_RANDOM_SHORT);
    }
  }
      
  void T2ITCStateOps_WCACHEXG::timeout(T2ITC & itc, T2PacketBuffer & pb, TimeQueue& tq) {
    if (itc.activeCircuitsInUse() > 0) // Shouldn't be, by now
      itc.reset();
    else if (itc.sendCacheAtoms(pb)) {  // True when all atoms shipped
      FAIL(INCOMPLETE_CODE);
    } else itc.scheduleWait(WC_RANDOM_SHORT);
  }
}
