#include "plugin.hpp"
#include "tubeunit_engine.hpp"

// Sapphire Tube Unit for VCV Rack 2, by Don Cross <cosinekitty@gmail.com>
// https://github.com/cosinekitty/sapphire


extern const std::vector<SapphireControlGroup> tubeUnitControls;


struct TubeUnitModule : Module
{
    Sapphire::TubeUnitEngine engine[PORT_MAX_CHANNELS];
    AgcLevelQuantity *agcLevelQuantity = nullptr;
    bool enableLimiterWarning = true;
    bool isInvertedVentPort = false;
    int numActiveChannels = 0;

    enum ParamId
    {
        // Large knobs for manual parameter adjustment
        AIRFLOW_PARAM,
        REFLECTION_DECAY_PARAM,
        REFLECTION_ANGLE_PARAM,
        STIFFNESS_PARAM,
        BYPASS_WIDTH_PARAM,
        BYPASS_CENTER_PARAM,
        ROOT_FREQUENCY_PARAM,
        VORTEX_PARAM,

        // Attenuverter knobs
        AIRFLOW_ATTEN,
        REFLECTION_DECAY_ATTEN,
        REFLECTION_ANGLE_ATTEN,
        STIFFNESS_ATTEN,
        BYPASS_WIDTH_ATTEN,
        BYPASS_CENTER_ATTEN,
        ROOT_FREQUENCY_ATTEN,
        VORTEX_ATTEN,

        // Parameters that do not participate in "control groups".
        LEVEL_KNOB_PARAM,
        AGC_LEVEL_PARAM,

        PARAMS_LEN
    };

    enum InputId
    {
        // Control group inputs
        AIRFLOW_INPUT,
        REFLECTION_DECAY_INPUT,
        REFLECTION_ANGLE_INPUT,
        STIFFNESS_INPUT,
        BYPASS_WIDTH_INPUT,
        BYPASS_CENTER_INPUT,
        ROOT_FREQUENCY_INPUT,
        VORTEX_INPUT,

        // Inputs that are not in a control group
        QUIET_GATE_INPUT,
        AUDIO_LEFT_INPUT,
        AUDIO_RIGHT_INPUT,

        INPUTS_LEN
    };

    enum OutputId
    {
        AUDIO_LEFT_OUTPUT,
        AUDIO_RIGHT_OUTPUT,

        OUTPUTS_LEN
    };

    enum LightId
    {
        LIGHTS_LEN
    };

    const SapphireControlGroup *cgLookup[INPUTS_LEN] {};

    TubeUnitModule()
    {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        for (const SapphireControlGroup& cg : tubeUnitControls)
        {
            assert(cgLookup[cg.inputId] == nullptr);
            cgLookup[cg.inputId] = &cg;
            configInput(cg.inputId, cg.name + " CV");
            configParam(cg.paramId, cg.minValue, cg.maxValue, cg.defaultValue, cg.name, cg.unit, cg.displayBase, cg.displayMultiplier);
            configParam(cg.attenId, -1, 1, 0, cg.name + " attenuverter", "%", 0, 100);
        }

        configOutput(AUDIO_LEFT_OUTPUT, "Left audio");
        configOutput(AUDIO_RIGHT_OUTPUT, "Right audio");

        agcLevelQuantity = configParam<AgcLevelQuantity>(
            AGC_LEVEL_PARAM,
            AGC_LEVEL_MIN,
            AGC_DISABLE_MAX,
            AGC_LEVEL_DEFAULT,
            "Output limiter"
        );
        agcLevelQuantity->value = AGC_LEVEL_DEFAULT;

        auto levelKnob = configParam(LEVEL_KNOB_PARAM, 0, 2, 1, "Output level", " dB", -10, 80);
        levelKnob->randomizeEnabled = false;

        configInput(QUIET_GATE_INPUT, "Vent gate");
        configInput(AUDIO_LEFT_INPUT, "Left audio");
        configInput(AUDIO_RIGHT_INPUT, "Right audio");

        configBypass(AUDIO_LEFT_INPUT,  AUDIO_LEFT_OUTPUT);
        configBypass(AUDIO_RIGHT_INPUT, AUDIO_RIGHT_OUTPUT);

        initialize();
    }

    void initialize()
    {
        numActiveChannels = 0;
        enableLimiterWarning = true;
        isInvertedVentPort = false;

        for (int c = 0; c < PORT_MAX_CHANNELS; ++c)
            engine[c].initialize();
    }

    void onReset(const ResetEvent& e) override
    {
        Module::onReset(e);
        initialize();
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();
        json_object_set_new(root, "limiterWarningLight", json_boolean(enableLimiterWarning));
        json_object_set_new(root, "toggleVentPort", json_boolean(isInvertedVentPort));
        return root;
    }

    void dataFromJson(json_t* root) override
    {
        // If the JSON is damaged, default to enabling the warning light.
        json_t *warningFlag = json_object_get(root, "limiterWarningLight");
        enableLimiterWarning = !json_is_false(warningFlag);

        // Upgrade from older/damaged JSON by defaulting the vent toggle to OFF.
        json_t *ventFlag = json_object_get(root, "toggleVentPort");
        isInvertedVentPort = json_is_true(ventFlag);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override
    {
        for (int c = 0; c < PORT_MAX_CHANNELS; ++c)
            engine[c].setSampleRate(e.sampleRate);
    }

    void onBypass(const BypassEvent& e) override
    {
        numActiveChannels = 0;
    }

    float getControlValue(InputId inputId, int cvChannel)
    {
        const SapphireControlGroup& cg = *cgLookup[inputId];
        float slider = params[cg.paramId].getValue();
        int nChannels = inputs[cg.inputId].getChannels();
        if (nChannels > 0)
        {
            int c = std::min(nChannels-1, cvChannel);
            float cv = inputs[cg.inputId].getVoltage(c);
            // When the attenuverter is set to 100%, and the cv is +5V, we want
            // to swing a slider that is all the way down (minSlider)
            // to act like it is all the way up (maxSlider).
            // Thus we allow the complete range of control for any CV whose
            // range is [-5, +5] volts.
            float attenu = params[cg.attenId].getValue();
            slider += attenu*(cv / 5)*(cg.maxValue - cg.minValue);
        }
        return Sapphire::Clamp(slider, cg.minValue, cg.maxValue);
    }

    void updateQuiet(int c)
    {
        bool quiet{};
        const int quietGateChannels = inputs[QUIET_GATE_INPUT].getChannels();
        if (c < quietGateChannels)
        {
            float qv = inputs[QUIET_GATE_INPUT].getVoltage(c);
            if (qv >= 1.0f)
                quiet = !isInvertedVentPort;
            else if (qv < 0.1f)
                quiet = isInvertedVentPort;
            else
                quiet = engine[c].getQuiet();
        }
        else if (quietGateChannels > 0)
            quiet = engine[quietGateChannels-1].getQuiet();
        else
            quiet = isInvertedVentPort;

        engine[c].setQuiet(quiet);
    }

    void process(const ProcessArgs& args) override
    {
        using namespace Sapphire;

        reflectAgcSlider();

        // Whichever input has the most channels selects the output channel count.
        // Other inputs have their final supplied value (or default value if none)
        // "normalled" to the remaining channels.
        numActiveChannels = std::max(1, inputs[QUIET_GATE_INPUT].getChannels());
        numActiveChannels = std::max(numActiveChannels, inputs[AUDIO_LEFT_INPUT].getChannels());
        numActiveChannels = std::max(numActiveChannels, inputs[AUDIO_RIGHT_INPUT].getChannels());
        for (const SapphireControlGroup& cg : tubeUnitControls)
            numActiveChannels = std::max(numActiveChannels, inputs[cg.inputId].getChannels());

        outputs[AUDIO_LEFT_OUTPUT ].setChannels(numActiveChannels);
        outputs[AUDIO_RIGHT_OUTPUT].setChannels(numActiveChannels);
        float leftIn = 0.0f;
        float rightIn = 0.0f;

        for (int c = 0; c < numActiveChannels; ++c)
        {
            updateQuiet(c);
            engine[c].setGain(params[LEVEL_KNOB_PARAM].getValue());
            engine[c].setAirflow(getControlValue(AIRFLOW_INPUT, c));
            engine[c].setRootFrequency(4 * std::pow(2.0f, getControlValue(ROOT_FREQUENCY_INPUT, c)));
            engine[c].setReflectionDecay(getControlValue(REFLECTION_DECAY_INPUT, c));
            engine[c].setReflectionAngle(M_PI * getControlValue(REFLECTION_ANGLE_INPUT, c));
            engine[c].setSpringConstant(0.005f * std::pow(10.0f, 4.0f * getControlValue(STIFFNESS_INPUT, c)));
            engine[c].setBypassWidth(getControlValue(BYPASS_WIDTH_INPUT, c));
            engine[c].setBypassCenter(getControlValue(BYPASS_CENTER_INPUT, c));
            engine[c].setVortex(getControlValue(VORTEX_INPUT, c));

            if (c < inputs[AUDIO_LEFT_INPUT].getChannels())
                leftIn = inputs[AUDIO_LEFT_INPUT].getVoltage(c) / 5.0f;

            if (c < inputs[AUDIO_RIGHT_INPUT].getChannels())
                rightIn = inputs[AUDIO_RIGHT_INPUT].getVoltage(c) / 5.0f;

            float leftOut, rightOut;
            engine[c].process(leftOut, rightOut, leftIn, rightIn);

            // Normalize TubeUnitEngine's dimensionless [-1, 1] output to VCV Rack's 5.0V peak amplitude.
            outputs[AUDIO_LEFT_OUTPUT ].setVoltage(5.0f * leftOut,  c);
            outputs[AUDIO_RIGHT_OUTPUT].setVoltage(5.0f * rightOut, c);
        }
    }

    void reflectAgcSlider()
    {
        // Check for changes to the automatic gain control: its level, and whether enabled/disabled.
        if (agcLevelQuantity && agcLevelQuantity->changed)
        {
            bool enabled = agcLevelQuantity->isAgcEnabled();
            for (int c = 0; c < PORT_MAX_CHANNELS; ++c)
            {
                if (enabled)
                    engine[c].setAgcLevel(agcLevelQuantity->clampedAgc() / 5.0f);
                engine[c].setAgcEnabled(enabled);
            }
            agcLevelQuantity->changed = false;
        }
    }

    float getAgcDistortion()
    {
        // Return the maximum distortion from the engines that are actively producing output.
        float maxDistortion = 0.0f;
        for (int c = 0; c < numActiveChannels; ++c)
        {
            float distortion = engine[c].getAgcDistortion();
            if (distortion > maxDistortion)
                maxDistortion = distortion;
        }
        return maxDistortion;
    }

    bool hasAudioInput()
    {
        return inputs[AUDIO_LEFT_INPUT].getChannels() + inputs[AUDIO_RIGHT_INPUT].getChannels() > 0;
    }
};


class TubeUnitWarningLightWidget : public LightWidget
{
private:
    TubeUnitModule *tubeUnitModule;

    static int colorComponent(double scale, int lo, int hi)
    {
        return clamp(static_cast<int>(round(lo + scale*(hi-lo))), lo, hi);
    }

    NVGcolor warningColor(double distortion)
    {
        bool enableWarning = tubeUnitModule && tubeUnitModule->enableLimiterWarning;

        if (!enableWarning || distortion <= 0.0)
            return nvgRGBA(0, 0, 0, 0);     // no warning light

        double decibels = 20.0 * std::log10(1.0 + distortion);
        double scale = clamp(decibels / 24.0);

        int red   = colorComponent(scale, 0x90, 0xff);
        int green = colorComponent(scale, 0x20, 0x50);
        int blue  = 0x00;
        int alpha = 0x70;

        return nvgRGBA(red, green, blue, alpha);
    }

public:
    explicit TubeUnitWarningLightWidget(TubeUnitModule *module)
        : tubeUnitModule(module)
    {
        borderColor = nvgRGBA(0x00, 0x00, 0x00, 0x00);      // don't draw a circular border
        bgColor     = nvgRGBA(0x00, 0x00, 0x00, 0x00);      // don't mess with the knob behind the light
    }

    void drawLayer(const DrawArgs& args, int layer) override
    {
        if (layer == 1)
        {
            // Update the warning light state dynamically.
            // Turn on the warning when the AGC is limiting the output.
            double distortion = tubeUnitModule ? tubeUnitModule->getAgcDistortion() : 0.0;
            color = warningColor(distortion);
        }
        LightWidget::drawLayer(args, layer);
    }
};


inline Vec TubeUnitKnobPos(float x, float y)
{
    return mm2px(Vec(20.5f + x*20.0f, 34.0f + y*21.0f - x*10.5f));
}


struct TubeUnitWidget : ModuleWidget
{
    TubeUnitModule *tubeUnitModule;
    TubeUnitWarningLightWidget *warningLight = nullptr;
    SapphireSvgOverlay *ventLabel = nullptr;
    SapphireSvgOverlay *sealLabel = nullptr;
    SapphireSvgOverlay *audioEmphasis = nullptr;

    explicit TubeUnitWidget(TubeUnitModule* module)
        : tubeUnitModule(module)
    {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/tubeunit.svg")));

        ventLabel = SapphireSvgOverlay::Load("res/tubeunit_vent.svg");
        addChild(ventLabel);

        sealLabel = SapphireSvgOverlay::Load("res/tubeunit_seal.svg");
        addChild(sealLabel);
        sealLabel->hide();

        audioEmphasis = SapphireSvgOverlay::Load("res/tubeunit_audio_path.svg");
        addChild(audioEmphasis);
        audioEmphasis->hide();

        // The control group text labels go in their own layer after (on top of) audioEmphasis,
        // so that when I show/hide the audioEmphasis layer, its slight opacity doesn't change
        // the contrast of the label text.
        addChild(SapphireSvgOverlay::Load("res/tubeunit_labels.svg"));

        // Audio output jacks
        Vec levelKnobPos = TubeUnitKnobPos(1, 4);
        const float outJackDx = 12.0;
        const float outJackDy =  5.0;
        Vec ljVec = levelKnobPos.plus(mm2px(Vec(+outJackDx, -outJackDy)));
        Vec rjVec = levelKnobPos.plus(mm2px(Vec(+outJackDx, +outJackDy)));
        addOutput(createOutputCentered<SapphirePort>(ljVec, module, TubeUnitModule::AUDIO_LEFT_OUTPUT));
        addOutput(createOutputCentered<SapphirePort>(rjVec, module, TubeUnitModule::AUDIO_RIGHT_OUTPUT));

        // Parameter knobs
        for (const SapphireControlGroup& cg : tubeUnitControls)
        {
            int xdir = 1 - 2*cg.xGrid;      // map xGrid=[0, 1] to a direction [+1, -1]

            Vec knobCenter = TubeUnitKnobPos(cg.xGrid, cg.yGrid);
            addParam(createParamCentered<RoundLargeBlackKnob>(knobCenter, tubeUnitModule, cg.paramId));

            Vec attenCenter = knobCenter.plus(mm2px(Vec(-10.0*xdir, -4.0)));
            addParam(createParamCentered<Trimpot>(attenCenter, tubeUnitModule, cg.attenId));

            Vec portCenter = knobCenter.plus(mm2px(Vec(-10.0*xdir, +4.0)));
            addInput(createInputCentered<SapphirePort>(portCenter, tubeUnitModule, cg.inputId));
        }

        RoundLargeBlackKnob *levelKnob = createParamCentered<RoundLargeBlackKnob>(levelKnobPos, module, TubeUnitModule::LEVEL_KNOB_PARAM);
        addParam(levelKnob);

        // Superimpose a warning light on the output level knob.
        // We turn the warning light on when one or more of the 16 limiters are distoring the output.
        warningLight = new TubeUnitWarningLightWidget(module);
        warningLight->box.pos  = Vec(0.0f, 0.0f);
        warningLight->box.size = levelKnob->box.size;
        levelKnob->addChild(warningLight);

        // Input gate for quieting the tube.
        addInput(createInputCentered<SapphirePort>(mm2px(Vec(10.5, 16.0)), module, TubeUnitModule::QUIET_GATE_INPUT));

        // Audio inputs (left and right).
        addInput(createInputCentered<SapphirePort>(mm2px(Vec( 9.0, 114.5)), module, TubeUnitModule::AUDIO_LEFT_INPUT));
        addInput(createInputCentered<SapphirePort>(mm2px(Vec(23.0, 114.5)), module, TubeUnitModule::AUDIO_RIGHT_INPUT));
    }

    void appendContextMenu(Menu* menu) override
    {
        if (tubeUnitModule != nullptr)
        {
            menu->addChild(new MenuSeparator);

            if (tubeUnitModule->agcLevelQuantity)
            {
                // Add slider to adjust the AGC's level setting (5V .. 10V) or to disable AGC.
                menu->addChild(new AgcLevelSlider(tubeUnitModule->agcLevelQuantity));

                // Add an option to enable/disable the warning slider.
                menu->addChild(createBoolPtrMenuItem<bool>("Limiter warning light", "", &tubeUnitModule->enableLimiterWarning));

                // Add toggle for whether the VENT port should be inverted to a SEAL port.
                menu->addChild(createBoolPtrMenuItem<bool>("Toggle VENT/SEAL", "", &tubeUnitModule->isInvertedVentPort));
            }
        }
    }

    void step() override
    {
        if (tubeUnitModule != nullptr)
        {
            // Toggle between showing "SEAL" or "VENT" depending on the toggle state.
            bool showSeal = tubeUnitModule->isInvertedVentPort;
            if (sealLabel->isVisible() != showSeal)
            {
                sealLabel->setVisible(showSeal);
                ventLabel->setVisible(!showSeal);
                tubeUnitModule->configInput(TubeUnitModule::QUIET_GATE_INPUT, showSeal ? "Seal gate" : "Vent gate");
            }

            // Update the visibility state of the emphasized border around certain pentagons,
            // depending on whether anything is connected to the audio input jacks.
            // This gives the user a clue that these three controls are the ones that
            // can affect audio input.
            audioEmphasis->setVisible(tubeUnitModule->hasAudioInput());
        }

        ModuleWidget::step();
    }
};

const std::vector<SapphireControlGroup> tubeUnitControls {
    {
        "Airflow", 0, 0,
        TubeUnitModule::AIRFLOW_PARAM,
        TubeUnitModule::AIRFLOW_ATTEN,
        TubeUnitModule::AIRFLOW_INPUT,
        0.0f, 5.0f, 1.0f
    },
    {
        "Vortex", 0, 1,
        TubeUnitModule::VORTEX_PARAM,
        TubeUnitModule::VORTEX_ATTEN,
        TubeUnitModule::VORTEX_INPUT,
        0.0f, 1.0f, 0.0f
    },
    {
        "Bypass width", 1, 0,
        TubeUnitModule::BYPASS_WIDTH_PARAM,
        TubeUnitModule::BYPASS_WIDTH_ATTEN,
        TubeUnitModule::BYPASS_WIDTH_INPUT,
        0.5f, 20.0f, 6.0f
    },
    {
        "Bypass center", 1, 1,
        TubeUnitModule::BYPASS_CENTER_PARAM,
        TubeUnitModule::BYPASS_CENTER_ATTEN,
        TubeUnitModule::BYPASS_CENTER_INPUT,
        -10.0f, +10.0f, 5.0f
    },
    {
        "Reflection decay", 2, 0,
        TubeUnitModule::REFLECTION_DECAY_PARAM,
        TubeUnitModule::REFLECTION_DECAY_ATTEN,
        TubeUnitModule::REFLECTION_DECAY_INPUT,
        0.0f, 1.0f, 0.5f
    },
    {
        "Reflection angle", 2, 1,
        TubeUnitModule::REFLECTION_ANGLE_PARAM,
        TubeUnitModule::REFLECTION_ANGLE_ATTEN,
        TubeUnitModule::REFLECTION_ANGLE_INPUT,
        0.0f, +1.0f, 0.1f
    },
    {
        "Root frequency", 3, 0,
        TubeUnitModule::ROOT_FREQUENCY_PARAM,
        TubeUnitModule::ROOT_FREQUENCY_ATTEN,
        TubeUnitModule::ROOT_FREQUENCY_INPUT,
        0.0f, 8.0f, 2.7279248f,
        " Hz", 2, 4
    },
    {
        "Stiffness", 3, 1,
        TubeUnitModule::STIFFNESS_PARAM,
        TubeUnitModule::STIFFNESS_ATTEN,
        TubeUnitModule::STIFFNESS_INPUT,
        0.0f, 1.0f, 0.5f
    },
};


Model* modelTubeUnit = createModel<TubeUnitModule, TubeUnitWidget>("TubeUnit");
