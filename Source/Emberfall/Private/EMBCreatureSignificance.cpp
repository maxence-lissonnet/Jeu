// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCreatureSignificance.h"

#include "Emberfall.h"
#include "EMBCreatureBase.h"
#include "EMBCreatureTypes.h"

#include "AIController.h"
#include "BrainComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "SignificanceManager.h"

namespace
{
	/** Étiquette sous laquelle les créatures sont enregistrées dans le gestionnaire. */
	const FName CreatureSignificanceTag(TEXT("EMBCreature"));

	/** Délai de grâce (s) sans point de vue avant de rendre une créature à son spawner (joueur en cours de chargement). */
	constexpr double NoViewpointGraceSeconds = 5.0;

	/**
	 * USignificanceManager::Get peut renvoyer un pointeur const selon la version du moteur : le const_cast est
	 * concentré ici (le gestionnaire est un objet du monde, jamais partagé en lecture seule).
	 */
	USignificanceManager* GetManager(const UWorld* World)
	{
		// INCERTITUDE : selon la version, Get prend un const UWorld* ou un UWorld*, et rend un pointeur const ou non.
		// Le const_cast du monde compile dans les deux cas (UWorld* se convertit en const UWorld*) ; celui du résultat
		// aussi (const_cast vers le même type est permis).
		return const_cast<USignificanceManager*>(USignificanceManager::Get(const_cast<UWorld*>(World)));
	}

	/**
	 * Significance d'une créature pour UN point de vue : plus elle est proche, plus elle est élevée. Le gestionnaire
	 * évalue chaque point de vue (un par joueur) et retient la valeur la plus haute : celle du joueur le plus proche.
	 * Fonction pure, sans état : appelée par USignificanceManager::Update.
	 */
	float CalculateCreatureSignificance(USignificanceManager::FManagedObjectInfo* ObjectInfo, const FTransform& Viewpoint)
	{
		const AActor* Actor = ObjectInfo ? Cast<AActor>(ObjectInfo->GetObject()) : nullptr;
		if (!Actor)
		{
			return 0.f;
		}
		const float Distance = static_cast<float>(FVector::Dist(Actor->GetActorLocation(), Viewpoint.GetLocation()));
		return FMath::Max(0.f, EMBCreatureDefaults::SignificanceMaxDistance - Distance);
	}
}

bool UEMBCreatureSignificance::ShouldCreateSubsystem(UObject* Outer) const
{
	// Mondes de jeu uniquement (pas les mondes d'aperçu de l'éditeur) ; serveur ET clients.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UEMBCreatureSignificance::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Un timer à 2 Hz plutôt qu'un Tick : le palier d'une créature n'a pas besoin d'être plus fin.
	InWorld.GetTimerManager().SetTimer(UpdateTimerHandle, this, &UEMBCreatureSignificance::UpdateSignificance,
		EMBCreatureDefaults::SignificanceUpdateInterval, true);
}

void UEMBCreatureSignificance::Deinitialize()
{
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}
	Creatures.Reset();
	Super::Deinitialize();
}

// ----------------------------------------------------------------------------------------------
// Registre et budget
// ----------------------------------------------------------------------------------------------

void UEMBCreatureSignificance::RegisterCreature(AEMBCreatureBase* Creature)
{
	if (!Creature || Creatures.Contains(Creature))
	{
		return;
	}
	Creatures.Add(Creature, EEMBCreatureDetailTier::Full);

	if (USignificanceManager* Manager = GetManager(GetWorld()))
	{
		Manager->RegisterObject(Creature, CreatureSignificanceTag, &CalculateCreatureSignificance);
	}
}

void UEMBCreatureSignificance::UnregisterCreature(AEMBCreatureBase* Creature)
{
	if (!Creature || !Creatures.Contains(Creature))
	{
		return;
	}
	Creatures.Remove(Creature);

	if (USignificanceManager* Manager = GetManager(GetWorld()))
	{
		Manager->UnregisterObject(Creature);
	}
}

int32 UEMBCreatureSignificance::GetLiveCreatureCount() const
{
	int32 Count = 0;
	for (const TPair<TWeakObjectPtr<AEMBCreatureBase>, EEMBCreatureDetailTier>& Pair : Creatures)
	{
		const AEMBCreatureBase* Creature = Pair.Key.Get();
		if (Creature && !Creature->IsDead())
		{
			++Count;
		}
	}
	return Count;
}

// ----------------------------------------------------------------------------------------------
// Paliers
// ----------------------------------------------------------------------------------------------

EEMBCreatureDetailTier UEMBCreatureSignificance::ComputeTier(const float Distance, const EEMBCreatureDetailTier Current)
{
	// Hystérésis de 250 cm : le palier courant est conservé jusqu'à sa limite + 250 cm (palier plus détaillé quitté
	// plus tard), et un palier plus détaillé n'est retrouvé qu'à sa limite - 250 cm. Exemple : une créature en palier
	// plein reste pleine jusqu'à 3 250 cm ; réduite, elle ne redevient pleine qu'en dessous de 2 750 cm. Évite
	// l'alternance pause / reprise de l'IA pour une créature à la limite.
	const float Margin = EMBCreatureDefaults::SignificanceHysteresis;
	const float FullLimit = EMBCreatureDefaults::FullDetailDistance + (Current == EEMBCreatureDetailTier::Full ? Margin : -Margin);
	const float ReducedLimit = EMBCreatureDefaults::ReducedDetailDistance + (Current != EEMBCreatureDetailTier::Paused ? Margin : -Margin);

	if (Distance < FullLimit)
	{
		return EEMBCreatureDetailTier::Full;
	}
	return Distance < ReducedLimit ? EEMBCreatureDetailTier::Reduced : EEMBCreatureDetailTier::Paused;
}

void UEMBCreatureSignificance::UpdateSignificance()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Points de vue : les joueurs LOCAUX de ce monde (tous les joueurs sur un serveur, le joueur local sur un client).
	TArray<FTransform> Viewpoints;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		if (Pawn)
		{
			Viewpoints.Add(FTransform(Pawn->GetActorLocation()));
		}
	}

	// Purge des créatures détruites.
	for (auto It = Creatures.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// Délai de grâce : sans aucun point de vue (joueur en chargement, reconnexion), on ne rend rien au spawner tout de suite.
	const double Now = World->GetTimeSeconds();
	if (Viewpoints.Num() > 0)
	{
		NoViewpointSince = -1.0;
	}
	else if (NoViewpointSince < 0.0)
	{
		NoViewpointSince = Now;
	}
	const bool bReleaseAllowed = Viewpoints.Num() > 0 || (Now - NoViewpointSince >= NoViewpointGraceSeconds);

	USignificanceManager* Manager = GetManager(World);
	if (!Manager && !bLoggedMissingManager)
	{
		bLoggedMissingManager = true;
		UE_LOG(LogEmberfall, Warning, TEXT("Significance : aucun USignificanceManager pour ce monde (classe non configuree ?). Distance calculee directement."));
	}
	if (Manager && Viewpoints.Num() > 0)
	{
		Manager->Update(Viewpoints);
	}

	TArray<AEMBCreatureBase*, TInlineAllocator<8>> ToRelease;
	for (TPair<TWeakObjectPtr<AEMBCreatureBase>, EEMBCreatureDetailTier>& Pair : Creatures)
	{
		AEMBCreatureBase* Creature = Pair.Key.Get();
		if (!Creature)
		{
			continue;
		}

		// Distance au joueur le plus proche : déduite de la significance du gestionnaire, sinon calculée directement.
		// Aucun joueur : distance maximale (créature en pause, éventuellement rendue au spawner).
		float Distance = EMBCreatureDefaults::SignificanceMaxDistance;
		float Significance = 0.f;
		if (Manager && Viewpoints.Num() > 0 && Manager->QuerySignificance(Creature, Significance))
		{
			Distance = EMBCreatureDefaults::SignificanceMaxDistance - Significance;
		}
		else
		{
			for (const FTransform& Viewpoint : Viewpoints)
			{
				Distance = FMath::Min(Distance, static_cast<float>(FVector::Dist(Creature->GetActorLocation(), Viewpoint.GetLocation())));
			}
		}

		const EEMBCreatureDetailTier NewTier = ComputeTier(Distance, Pair.Value);
		if (NewTier != Pair.Value)
		{
			Pair.Value = NewTier;
			ApplyTier(*Creature, NewTier);
		}

		// Trop loin de tout joueur : rendue au spawner (serveur, créature issue d'un spawner, pas en pleine attaque).
		if (bReleaseAllowed && Distance > EMBCreatureDefaults::ReleaseDistance && Creature->HasAuthority()
			&& Creature->HasOwningSpawner() && !Creature->IsDead() && !Creature->IsAttacking())
		{
			ToRelease.Add(Creature);
		}
	}

	// Hors de la boucle : ReturnToSpawner détruit l'acteur, ce qui modifie le registre (EndPlay).
	for (AEMBCreatureBase* Creature : ToRelease)
	{
		Creature->ReturnToSpawner();
	}
}

void UEMBCreatureSignificance::ApplyTier(AEMBCreatureBase& Creature, const EEMBCreatureDetailTier Tier) const
{
	if (Creature.IsDead())
	{
		return; // un cadavre garde son animation / son ragdoll
	}

	// --- Animation (serveur ET clients) ---
	if (USkeletalMeshComponent* MeshComp = Creature.GetMesh())
	{
		switch (Tier)
		{
		case EEMBCreatureDetailTier::Full:
			MeshComp->SetComponentTickEnabled(true);
			MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			MeshComp->bEnableUpdateRateOptimizations = false;
			break;
		case EEMBCreatureDetailTier::Reduced:
			// Seuls les montages (attaques) restent évalués hors écran ; URO espace les mises à jour de pose.
			MeshComp->SetComponentTickEnabled(true);
			MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
			MeshComp->bEnableUpdateRateOptimizations = true;
			break;
		case EEMBCreatureDetailTier::Paused:
			MeshComp->SetComponentTickEnabled(false);
			break;
		}
	}

	// --- IA (serveur uniquement : les contrôleurs n'existent pas chez les clients) ---
	AAIController* AIController = Cast<AAIController>(Creature.GetController());
	if (!AIController)
	{
		return;
	}
	UBrainComponent* Brain = AIController->FindComponentByClass<UBrainComponent>();

	// Perception : désactivée en palier pausé (aucune requête de vue / d'ouïe pour une créature endormie), réactivée sinon.
	if (UAIPerceptionComponent* Perception = AIController->GetAIPerceptionComponent())
	{
		const bool bSense = Tier != EEMBCreatureDetailTier::Paused;
		Perception->SetSenseEnabled(UAISense_Sight::StaticClass(), bSense);
		Perception->SetSenseEnabled(UAISense_Hearing::StaticClass(), bSense);
	}

	// Réseau : fréquence de mise à jour réduite hors palier plein (créature lointaine, mouvement peu lisible).
	Creature.SetNetUpdateFrequency(Tier == EEMBCreatureDetailTier::Full ? 20.f : 5.f);

	switch (Tier)
	{
	case EEMBCreatureDetailTier::Full:
	case EEMBCreatureDetailTier::Reduced:
	{
		const float Interval = (Tier == EEMBCreatureDetailTier::Reduced) ? EMBCreatureDefaults::ReducedAITickInterval : 0.f;
		AIController->SetActorTickInterval(Interval); // 0 = tick à chaque frame
		if (Brain)
		{
			Brain->SetComponentTickInterval(Interval);
			if (Brain->IsPaused())
			{
				Brain->ResumeLogic(TEXT("Significance"));
			}
		}
		break;
	}
	case EEMBCreatureDetailTier::Paused:
		AIController->StopMovement();
		if (Brain && !Brain->IsPaused())
		{
			Brain->PauseLogic(TEXT("Significance"));
		}
		break;
	}
}
