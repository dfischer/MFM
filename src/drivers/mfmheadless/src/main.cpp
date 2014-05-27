#include "main.h"

namespace MFM
{
  typedef ParamConfig<64,4,8,40> OurParamConfig;
  typedef P3Atom<OurParamConfig> OurAtom;
  typedef CoreConfig<OurAtom, OurParamConfig> OurCoreConfig;
  typedef GridConfig<OurCoreConfig, 5, 3> OurGridConfig;

  struct MFMSimHeadlessDemo : public AbstractHeadlessDriver<OurGridConfig>
  {
    MFMSimHeadlessDemo(DriverArguments& args) : AbstractHeadlessDriver(args) { }

    virtual void ReinitPhysics()
    {
      OurGrid& m_grid = GetGrid();

      m_grid.Needed(Element_Dreg<OurCoreConfig>::THE_INSTANCE);
      m_grid.Needed(Element_Res<OurCoreConfig>::THE_INSTANCE);
    }

    void ReinitEden()
    {
      OurGrid& m_grid = GetGrid();

      OurAtom dreg(Element_Dreg<OurCoreConfig>::THE_INSTANCE.GetDefaultAtom());

      SPoint dregPt(30, 30);

      m_grid.PlaceAtom(dreg, dregPt);

    }
  };
}

int main(int argc, char** argv)
{
  MFM::LOG.SetByteSink(MFM::STDERR);
  MFM::LOG.SetLevel(MFM::LOG.ALL);

  MFM::DriverArguments args(argc, argv);
  MFM::MFMSimHeadlessDemo sim(args);

  sim.ReinitUs();

  sim.Run();

  return 0;
}
