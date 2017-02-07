#include "JetImageBuffer.h"

struct cout_redirect {
        cout_redirect(std::streambuf *new_buffer): old(std::cout.rdbuf(new_buffer))
        {}

        ~cout_redirect() {
            std::cout.rdbuf(old);
        }

    private:
        std::streambuf *old;
};

// Constructor
JetImageBuffer::JetImageBuffer(int imagesize, bool debug) {
    m_console->set_pattern("[%Y-%m-%d %T.%f] [%n] [pid=%P] [%l] %v");
    if (debug) {
        m_console->set_level(spdlog::level::debug);
    }

    imagesize *= imagesize;
    MaxN = imagesize;
    m_Intensity = new float[imagesize];

    m_console->debug("JetImageBuffer::JetImageBuffer - building idealized detector");

    ftest = 0;
    fOutName = "test.root";

    // model the detector as a 2D histogram, with 100 x bins and 200 y bins
    detector = new TH2F("", "", 100, -5, 5, 200, -10, 10);
    for (unsigned int i = 1; i <= 100; i++) {
        for (unsigned int j = 1; j <= 200; j++) {
            detector->SetBinContent(i, j, 0);
        }
    }

    m_console->debug("JetImageBuffer::JetImageBuffer - idealized detector built");
}

// Destructor
JetImageBuffer::~JetImageBuffer() {
    m_console->debug("JetImageBuffer::~JetImageBuffer - calling destructor");
    // delete tool;
    delete[] m_Intensity;
}

// Begin method
void JetImageBuffer::Begin() {
    // Declare TTree
    m_console->debug("JetImageBuffer::Begin - storing simulation into " + fOutName + "/EventTree");
    tF = new TFile(fOutName.c_str(), "RECREATE");
    tT = new TTree("EventTree", "Event Tree for MI");

    // for shit you want to do by hand
    DeclareBranches();
    ResetBranches();

    return;
}

// End
void JetImageBuffer::End() {
    tT->Write();
    tF->Close();
    return;
}

// Analyze
void JetImageBuffer::AnalyzeEvent(int ievt, Pythia8::Pythia *pythia8,
                                  Pythia8::Pythia *pythia_MB, int NPV,
                                  int pixels, float range) {

    std::stringstream buf;
    cout_redirect redirect(buf.rdbuf());

    if (!pythia8->next()) {
        m_console->warn("JetImageBuffer::AnalyzeEvent[will continue, skipping] - NULL retval from pythia8->next()");
        return;
    }
    if (!buf.str().empty()) {

        auto err_str = buf.str();
        err_str.erase(std::remove(err_str.begin(), err_str.end(), '\n'), err_str.end());
        if ((err_str.find("Error") != std::string::npos) || \
                (err_str.find("error") != std::string::npos)) {
            m_console->error("JetImageBuffer::AnalyzeEvent[will continue, skipping] - evt#" + \
                             std::to_string(ievt) + ": "  + err_str);
        } else {
            m_console->warn("JetImageBuffer::AnalyzeEvent[will continue, skipping] - evt#" + \
                            std::to_string(ievt) + ": "  + err_str);
        }

    }

    if (ievt % 1000 == 0) {
        m_console->debug("JetImageBuffer::AnalyzeEvent - simulation event #" + std::to_string(ievt));
    }

    // reset branches
    ResetBranches();

    // new event-----------------------
    evt_number = ievt;
    std::vector<fastjet::PseudoJet> particlesForJets;
    std::vector<fastjet::PseudoJet> particlesForJets_nopixel;

    detector->Reset();

    // Particle loop
    for (int ip = 0; ip < pythia8->event.size(); ++ip) {
        fastjet::PseudoJet jet(pythia8->event[ip].px(), pythia8->event[ip].py(),
                               pythia8->event[ip].pz(), pythia8->event[ip].e());

        // particles for jets
        if (!pythia8->event[ip].isFinal()) {
            continue;
        }

        // Skip neutrinos, PDGid = 12, 14, 16
        auto pdgid = std::abs(pythia8->event[ip].id());

        if (pdgid == 12 || pdgid == 14 || pdgid == 16) {
            continue;
        }

        // find the particles rapidity and phi, then get the detector bins
        int ybin = detector->GetXaxis()->FindBin(jet.rapidity());
        int phibin = detector->GetYaxis()->FindBin(jet.phi());

        // do bin += value in the associated detector bin
        detector->SetBinContent(ybin, phibin, detector->GetBinContent(ybin, phibin) + jet.e());

        fastjet::PseudoJet p_nopix(jet.px(), jet.py(), jet.pz(), jet.e());
        particlesForJets_nopixel.push_back(p_nopix);
    }
    // end particle loop -----------------------------------------------

    // Now, we extract the energy from the calorimeter for processing by fastjet
    for (int i = 1; i <= detector->GetNbinsX(); i++) {
        for (int j = 1; j <= detector->GetNbinsY(); j++) {
            if (detector->GetBinContent(i, j) > 0) {
                double phi = detector->GetYaxis()->GetBinCenter(j);
                double eta = detector->GetXaxis()->GetBinCenter(i);
                double E = detector->GetBinContent(i, j);
                fastjet::PseudoJet p(0., 0., 0., 0.);

                // We measure E (not pT)!  And treat 'clusters' as massless.
                p.reset_PtYPhiM(E / cosh(eta), eta, phi, 0.);
                particlesForJets.push_back(p);
            }
        }
    }

    fastjet::JetDefinition *m_jet_def =
        new fastjet::JetDefinition(fastjet::antikt_algorithm, 1.0);

    fastjet::Filter trimmer(fastjet::JetDefinition(fastjet::kt_algorithm, 0.3),
                            fastjet::SelectorPtFractionMin(0.05));

    // Silence the FastJet banner
    std::cout.setstate(std::ios::failbit);
    fastjet::ClusterSequence csLargeR(particlesForJets, *m_jet_def);
    fastjet::ClusterSequence csLargeR_nopix(particlesForJets_nopixel,
                                            *m_jet_def);
    // reset std::cout to its normal state
    std::cout.clear();

    std::vector<fastjet::PseudoJet> considered_jets =
        fastjet::sorted_by_pt(csLargeR.inclusive_jets(10.0));
    std::vector<fastjet::PseudoJet> considered_jets_nopix =
        fastjet::sorted_by_pt(csLargeR_nopix.inclusive_jets(10.0));
    fastjet::PseudoJet leading_jet = trimmer(considered_jets[0]);
    fastjet::PseudoJet leading_jet_nopix = trimmer(considered_jets_nopix[0]);

    m_LeadingEta = leading_jet.eta();
    m_LeadingPhi = leading_jet.phi();
    m_LeadingPt = leading_jet.perp();
    m_LeadingM = leading_jet.m();
    m_LeadingEta_nopix = leading_jet_nopix.eta();
    m_LeadingPhi_nopix = leading_jet_nopix.phi();
    m_LeadingPt_nopix = leading_jet_nopix.perp();
    m_LeadingM_nopix = leading_jet_nopix.m();

    m_deltaR = 0.;
    if (leading_jet.pieces().size() > 1) {
        std::vector<fastjet::PseudoJet> subjets = leading_jet.pieces();
        TLorentzVector l(subjets[0].px(), subjets[0].py(), subjets[0].pz(),
                         subjets[0].E());
        TLorentzVector sl(subjets[1].px(), subjets[1].py(), subjets[1].pz(),
                          subjets[1].E());
        m_deltaR = l.DeltaR(sl);
        m_SubLeadingEta = sl.Eta() - l.Eta();
        m_SubLeadingPhi = subjets[1].delta_phi_to(subjets[0]);
    }

    std::vector<std::pair<double, double>> consts_image;
    std::vector<fastjet::PseudoJet> sorted_consts =
        sorted_by_pt(leading_jet.constituents());

    for (unsigned int i = 0; i < sorted_consts.size(); i++) {
        std::pair<double, double> const_hold;
        const_hold.first = sorted_consts[i].eta();
        const_hold.second = sorted_consts[i].phi();
        consts_image.push_back(const_hold);
    }

    std::vector<fastjet::PseudoJet> subjets = leading_jet.pieces();

    // Step 1: Center on the jet axis.
    for (unsigned int i = 0; i < sorted_consts.size(); i++) {
        consts_image[i].first = consts_image[i].first - subjets[0].eta();
        consts_image[i].second =
            sorted_consts[i].delta_phi_to(subjets[0]); // use delta phi to take
        // care of the
        // dis-continuity in phi
    }

    // Quickly run PCA for the rotation.
    double xbar = 0.;
    double ybar = 0.;
    double x2bar = 0.;
    double y2bar = 0.;
    double xybar = 0.;
    double n = 0;

    for (unsigned int i = 0; i < leading_jet.constituents().size(); i++) {
        double x = consts_image[i].first;
        double y = consts_image[i].second;
        double E = sorted_consts[i].e();
        n += E;
        xbar += x * E;
        ybar += y * E;
    }

    double mux = xbar / n;
    double muy = ybar / n;

    xbar = 0.;
    ybar = 0.;
    n = 0.;

    for (unsigned int i = 0; i < leading_jet.constituents().size(); i++) {
        double x = consts_image[i].first - mux;
        double y = consts_image[i].second - muy;
        double E = sorted_consts[i].e();
        n += E;
        xbar += x * E;
        ybar += y * E;
        x2bar += x * x * E;
        y2bar += y * y * E;
        xybar += x * y * E;
    }

    double sigmax2 = x2bar / n - mux * mux;
    double sigmay2 = y2bar / n - muy * muy;
    double sigmaxy = xybar / n - mux * muy;
    double lamb_min = 0.5 * (sigmax2 + sigmay2 -
                             sqrt((sigmax2 - sigmay2) * (sigmax2 - sigmay2) +
                                  4 * sigmaxy * sigmaxy));

    double dir_x = sigmax2 + sigmaxy - lamb_min;
    double dir_y = sigmay2 + sigmaxy - lamb_min;

    // The first PC is only defined up to a sign.  Let's have it point toward
    // the
    // side of the jet with the most energy.

    double Eup = 0.;
    double Edn = 0.;

    for (unsigned int i = 0; i < leading_jet.constituents().size(); i++) {
        double x = consts_image[i].first - mux;
        double y = consts_image[i].second - muy;
        double E = sorted_consts[i].e();
        double dotprod = dir_x * x + dir_y * y;
        if (dotprod > 0)
            Eup += E;
        else
            Edn += E;
    }

    if (Edn < Eup) {
        dir_x = -dir_x;
        dir_y = -dir_y;
    }

    m_PCEta = dir_x;
    m_PCPhi = dir_y;

    // Step 2: Fill in the unrotated image
    //-------------------------------------------------------------------------
    TH2F *orig_im =
        new TH2F("", "", pixels, -range, range, pixels, -range, range);

    for (unsigned int i = 0; i < sorted_consts.size(); i++) {
        TLorentzVector hold = TLorentzVector();

        hold.SetPtEtaPhiM(sorted_consts[i].perp(), consts_image[i].first,
                          consts_image[i].second, 0.0);

        orig_im->Fill(consts_image[i].first, consts_image[i].second, hold.E());
        // std::cout << i << "       " << consts_image[i].first  << " " <<
        // consts_image[i].second << std::endl;
    }

    // Step 5: Dump the images in the tree!
    //-------------------------------------------------------------------------
    int counter = 0;
    for (int i = 1; i <= orig_im->GetNbinsX(); i++) {
        for (int j = 1; j <= orig_im->GetNbinsY(); j++) {
            // m_RotatedIntensity[counter] = rotatedimage->GetBinContent(i,j);
            double eta_disc = orig_im->GetXaxis()->GetBinCenter(i);
            m_Intensity[counter] = orig_im->GetBinContent(i, j) / cosh(eta_disc);
            // m_LocalDensity[counter] = localdensity->GetBinContent(i, j);
            // m_GlobalDensity[counter] = globaldensity->GetBinContent(i, j);

            counter++;
        }
    }

    // Step 6: Fill in nsubjettiness (new)
    //----------------------------------------------------------------------------
    fastjet::contrib::OnePass_WTA_KT_Axes axis_spec;
    fastjet::contrib::NormalizedMeasure parameters(1.0, 1.0);

    // NormalizedMeasure parameters(1.0, 1.0);
    fastjet::contrib::Nsubjettiness subjettiness_1(1, axis_spec, parameters);
    fastjet::contrib::Nsubjettiness subjettiness_2(2, axis_spec, parameters);
    fastjet::contrib::Nsubjettiness subjettiness_3(3, axis_spec, parameters);

    m_Tau1 = (float)subjettiness_1.result(leading_jet);
    m_Tau2 = (float)subjettiness_2.result(leading_jet);
    m_Tau3 = (float)subjettiness_3.result(leading_jet);

    m_Tau32 = (std::abs(m_Tau2) < 1e-4 ? -10 : m_Tau3 / m_Tau2);
    m_Tau21 = (std::abs(m_Tau1) < 1e-4 ? -10 : m_Tau2 / m_Tau1);

    m_Tau1_nopix = (float)subjettiness_1.result(leading_jet_nopix);
    m_Tau2_nopix = (float)subjettiness_2.result(leading_jet_nopix);
    m_Tau3_nopix = (float)subjettiness_3.result(leading_jet_nopix);

    m_Tau32_nopix =
        (std::abs(m_Tau2_nopix) < 1e-4 ? -10 : m_Tau3_nopix / m_Tau2_nopix);
    m_Tau21_nopix =
        (std::abs(m_Tau1_nopix) < 1e-4 ? -10 : m_Tau2_nopix / m_Tau1_nopix);

    tT->Fill();

    return;
}

// declate branches
void JetImageBuffer::DeclareBranches() {
    // Event Properties
    tT->Branch("NFilled", &m_NFilled, "NFilled/I");

    tT->Branch("Intensity", * &m_Intensity, "Intensity[NFilled]/F");

    tT->Branch("SubLeadingEta", &m_SubLeadingEta, "SubLeadingEta/F");
    tT->Branch("SubLeadingPhi", &m_SubLeadingPhi, "SubLeadingPhi/F");

    tT->Branch("PCEta", &m_PCEta, "PCEta/F");
    tT->Branch("PCPhi", &m_PCPhi, "PCPhi/F");

    tT->Branch("LeadingEta", &m_LeadingEta, "LeadingEta/F");
    tT->Branch("LeadingPhi", &m_LeadingPhi, "LeadingPhi/F");
    tT->Branch("LeadingPt", &m_LeadingPt, "LeadingPt/F");
    tT->Branch("LeadingM", &m_LeadingM, "LeadingM/F");

    tT->Branch("LeadingEta_nopix", &m_LeadingEta_nopix, "LeadingEta_nopix/F");
    tT->Branch("LeadingPhi_nopix", &m_LeadingPhi_nopix, "LeadingPhi_nopix/F");
    tT->Branch("LeadingPt_nopix", &m_LeadingPt_nopix, "LeadingPt_nopix/F");
    tT->Branch("LeadingM_nopix", &m_LeadingM_nopix, "LeadingM_nopix/F");

    tT->Branch("Tau1", &m_Tau1, "Tau1/F");
    tT->Branch("Tau2", &m_Tau2, "Tau2/F");
    tT->Branch("Tau3", &m_Tau3, "Tau3/F");

    tT->Branch("Tau1_nopix", &m_Tau1_nopix, "Tau1_nopix/F");
    tT->Branch("Tau2_nopix", &m_Tau2_nopix, "Tau2_nopix/F");
    tT->Branch("Tau3_nopix", &m_Tau3_nopix, "Tau3_nopix/F");

    tT->Branch("DeltaR", &m_deltaR, "DeltaR/F");

    tT->Branch("Tau32", &m_Tau32, "Tau32/F");
    tT->Branch("Tau21", &m_Tau21, "Tau21/F");

    tT->Branch("Tau32_nopix", &m_Tau32_nopix, "Tau32_nopix/F");
    tT->Branch("Tau21_nopix", &m_Tau21_nopix, "Tau21_nopix/F");
    return;
}

// resets vars
void JetImageBuffer::ResetBranches() {
    // reset branches
    m_NFilled = MaxN;
    m_SubLeadingPhi = -999;
    m_SubLeadingEta = -999;
    m_PCPhi = -999;
    m_PCEta = -999;
    m_Tau32 = -999;
    m_Tau21 = -999;

    m_Tau1 = -999;
    m_Tau2 = -999;
    m_Tau3 = -999;

    m_Tau32_nopix = -999;
    m_Tau21_nopix = -999;

    m_Tau1_nopix = -999;
    m_Tau2_nopix = -999;
    m_Tau3_nopix = -999;

    m_LeadingEta = -999;
    m_LeadingPhi = -999;
    m_LeadingPt = -999;
    m_LeadingM = -999;

    m_LeadingEta_nopix = -999;
    m_LeadingPhi_nopix = -999;
    m_LeadingPt_nopix = -999;
    m_LeadingM_nopix = -999;

    for (int iP = 0; iP < MaxN; ++iP) {
        m_Intensity[iP] = -999;
    }
}
