/**
 *  @file   LCContent/src/LCReclustering/ForceSplitTrackAssociationsAlg.cc
 * 
 *  @brief  Implementation of the force split track associations algorithm class.
 * 
 *  $Log: $
 */

#include "Pandora/AlgorithmHeaders.h"

#include "LCReclustering/ForceSplitTrackAssociationsAlg.h"

using namespace pandora;

namespace lc_content
{

ForceSplitTrackAssociationsAlg::ForceSplitTrackAssociationsAlg() :
    m_minTrackAssociations(2)
{
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode ForceSplitTrackAssociationsAlg::Run()
{
    const ClusterList *pClusterList = NULL;
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::GetCurrentList(*this, pClusterList));

    const float bField(PandoraContentApi::GetPlugins(*this)->GetBFieldPlugin()->GetBField(CartesianVector(0.f, 0.f, 0.f)));

    // Loop over clusters in the algorithm input list, looking for those with excess track associations
    for (ClusterList::const_iterator iter = pClusterList->begin(); iter != pClusterList->end();)
    {
        const Cluster *const pOriginalCluster = *iter;
        ++iter;

        const TrackList trackList(pOriginalCluster->GetAssociatedTrackList());

        if (trackList.size() < m_minTrackAssociations)
            continue;

        OrderedCaloHitList orderedCaloHitList(pOriginalCluster->GetOrderedCaloHitList());
        PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, orderedCaloHitList.Add(pOriginalCluster->GetIsolatedCaloHitList()));

        // Initialize cluster fragmentation operations
        const ClusterList clusterList(1, pOriginalCluster);
        std::string originalClustersListName, fragmentClustersListName;

        PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::InitializeFragmentation(*this, clusterList,
            originalClustersListName, fragmentClustersListName));

        // Remove original track-cluster associations and create new track-seeded clusters for each track
        TrackToClusterMap trackToClusterMap;
        TrackToHelixMap trackToHelixMap;

        for (TrackList::const_iterator trackIter = trackList.begin(), trackIterEnd = trackList.end(); trackIter != trackIterEnd;
            ++trackIter)
        {
            const Track *const pTrack = *trackIter;
            const Helix helix(pTrack->GetTrackStateAtCalorimeter().GetPosition(), pTrack->GetTrackStateAtCalorimeter().GetMomentum(), pTrack->GetCharge(), bField);

            PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::RemoveTrackClusterAssociation(*this, pTrack, pOriginalCluster));

            const Cluster *pCluster = NULL;
            PandoraContentApi::Cluster::Parameters parameters;
            parameters.m_pTrack = pTrack;
            PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::Cluster::Create(*this, parameters, pCluster));

            if (!trackToClusterMap.insert(TrackToClusterMap::value_type(pTrack, pCluster)).second ||
                !trackToHelixMap.insert(TrackToHelixMap::value_type(pTrack, helix)).second)
            {
                return STATUS_CODE_FAILURE;
            }
        }

        // Assign the calo hits in the original cluster to the most appropriate track
        for (OrderedCaloHitList::const_iterator listIter = orderedCaloHitList.begin(), listIterEnd = orderedCaloHitList.end();
            listIter != listIterEnd; ++listIter)
        {
            for (CaloHitList::const_iterator hitIter = listIter->second->begin(), hitIterEnd = listIter->second->end();
                hitIter != hitIterEnd; ++hitIter)
            {
                const CaloHit *const pCaloHit = *hitIter;
                const CartesianVector &hitPosition(pCaloHit->GetPositionVector());

                // Identify most suitable cluster for calo hit, using distance to helix fit as figure of merit
                const Cluster *pBestCluster = NULL;
                float bestClusterEnergy(0.);
                float minDistanceToTrack(std::numeric_limits<float>::max());

                for (TrackToClusterMap::const_iterator mapIter = trackToClusterMap.begin(), mapIterEnd = trackToClusterMap.end();
                    mapIter != mapIterEnd; ++mapIter)
                {
                    const Track *const pTrack(mapIter->first);
                    const Cluster *const pCluster(mapIter->second);

                    TrackToHelixMap::const_iterator helixIter = trackToHelixMap.find(pTrack);

                    if (trackToHelixMap.end() == helixIter)
                        return STATUS_CODE_FAILURE;

                    const Helix &helix(helixIter->second);

                    CartesianVector helixSeparation(0.f, 0.f, 0.f);
                    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, helix.GetDistanceToPoint(hitPosition, helixSeparation));

                    const float distanceToTrack(helixSeparation.GetMagnitude());
                    const float clusterEnergy(pCluster->GetHadronicEnergy());

                    if ((distanceToTrack < minDistanceToTrack) || ((distanceToTrack == minDistanceToTrack) && (clusterEnergy > bestClusterEnergy)))
                    {
                        minDistanceToTrack = distanceToTrack;
                        pBestCluster = pCluster;
                        bestClusterEnergy = clusterEnergy;
                    }
                }

                if (NULL == pBestCluster)
                {
                    return STATUS_CODE_FAILURE;
                }

                if (!pCaloHit->IsIsolated())
                {
                    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::AddToCluster(*this, pBestCluster, pCaloHit));
                }
                else
                {
                    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::AddIsolatedToCluster(*this, pBestCluster, pCaloHit));
                }
            }
        }

        // Check for any "empty" clusters and create new track-cluster associations
        for (TrackToClusterMap::iterator mapIter = trackToClusterMap.begin(); mapIter != trackToClusterMap.end();)
        {
            const Cluster *const pCluster = mapIter->second;

            if (0 == pCluster->GetNCaloHits())
            {
                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::Delete(*this, pCluster));
                trackToClusterMap.erase(mapIter++);
            }
            else
            {
                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::AddTrackClusterAssociation(*this, mapIter->first, pCluster));
                ++mapIter;
            }
        }

        if (trackToClusterMap.empty())
        {
            return STATUS_CODE_FAILURE;
        }

        // End cluster fragmentation operations, automatically choose the new cluster fragments
        PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::EndFragmentation(*this, fragmentClustersListName,
            originalClustersListName));
    }

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode ForceSplitTrackAssociationsAlg::ReadSettings(const TiXmlHandle xmlHandle)
{
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MinTrackAssociations", m_minTrackAssociations));

    if (m_minTrackAssociations < 2)
        return STATUS_CODE_INVALID_PARAMETER;

    return STATUS_CODE_SUCCESS;
}

} // namespace lc_content
